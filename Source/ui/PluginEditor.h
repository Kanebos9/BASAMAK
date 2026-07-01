#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// Draws the 8×N step grid. Steps fill the component's actual width, so the
// grid stretches with the window and fewer steps means wider buttons.
// Each step also shows its MIDI assignment ("ch6cc44" / "no midi").
class StepGridComponent : public juce::Component
{
public:
    // Step edit mode: 0 = on/off, 1 = Velocity, 2 = Pitch, 3 = Probability, 4 = Roll, 5 = Pan.
    enum EditMode { ModeSteps = 0, ModeVel, ModePitch, ModeProb, ModeRoll, ModePan };
    int editMode = ModeSteps;
    bool influenceArmed[Sequencer::NUM_CHANNELS] = {};   // per channel: next touched step propagates to all

    std::function<void(int ch, int step)> onStepClicked;
    std::function<void(int ch)>           onChannelSelected;
    std::function<int()>                  getLearnChannel; // current pattern's channel
    // Called when a value (velocity/pitch/probability) is edited in a value mode.
    std::function<void(int ch, int step, int mode, float value)> onStepValueChanged;
    // Called when the per-step LOOP condition (Prob mode) is edited: cycle length + enabled-loop bitmask.
    std::function<void(int ch, int step, int len, int mask)> onStepCondChanged;
    // Influence: copy one source step's vel/pitch/prob/roll onto every step in the channel.
    std::function<void(int ch, int srcStep)> onInfluenceApply;
    std::function<void(int ch)>              onInfluenceDisarm; // un-highlight the strip button
    MidiLearnManager* midiLearn = nullptr;

    int rowH = 44;            // set by parent layout
    int visibleRows = 8;      // how many channel rows to draw at once (the viewport height in rows)
    int firstRow = 0;         // scroll offset: the topmost drawn channel (maps ch -> screen row ch-firstRow)
    int currentPattern = 0;   // which pattern's steps/assignments we display

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;   // value modes: reset the step to its default

    void update(const Sequencer& seq, bool anySolo);
    float getRollDec(int ch, int step) const { return rollDec[ch][step]; }
    float getVel(int ch, int step) const { return vel[ch][step]; }   // velocity set by X-drag in Roll mode
    float getNoteLen(int ch, int step) const { return noteLen[ch][step]; }  // length set by X in Vel/Len mode (MIDI)

private:
    static constexpr int NCH = Sequencer::NUM_CHANNELS;
    bool  steps[NCH][DrumChannel::MAX_STEPS] = {};
    float vel[NCH][DrumChannel::MAX_STEPS]   = {};
    float pit[NCH][DrumChannel::MAX_STEPS]   = {};
    float prob[NCH][DrumChannel::MAX_STEPS]  = {};
    int   roll[NCH][DrumChannel::MAX_STEPS]  = {};
    float rollDec[NCH][DrumChannel::MAX_STEPS] = {};   // roll decay 0..1 (fade across ratchet hits)
    float noteLen[NCH][DrumChannel::MAX_STEPS] = {};   // per-step MIDI note length 0..1 (Vel/Len mode)
    float pan[NCH][DrumChannel::MAX_STEPS]   = {};      // per-step stereo pan -1..+1 (Pan mode; 0 = centre)
    int   condLen[NCH][DrumChannel::MAX_STEPS]  = {};   // per-step loop-condition cycle length (Prob mode)
    int   condMask[NCH][DrumChannel::MAX_STEPS] = {};   // per-step loop-condition bitmask (0 = every loop)
    int   condDragCh = -1, condDragStep = -1, condDownBar = -1, condDownX = 0;   // Prob-mode gesture state
    bool  condDragged = false;
    int   curLoop = 0;          // the playing pattern's loop counter (highlights the current bar in Prob mode)
    bool  midiOutCh[NCH] = {};                         // is this channel routed to MIDI Out? (enables 2D Vel/Len)
    bool  muted[NCH]       = {};
    bool  soloed[NCH]      = {};
    int   numSteps[NCH]    = {};   // filled by update(); 0 until then
    int   playStep[NCH]    = {};   // filled by update(); 0 here is harmless (no cursor drawn until playing)
    bool  anySolo        = false;
    int   dragChannel    = -1;   // channel of the in-progress value drag (for Influence)
    int   dragStep       = -1;   // step the value drag started on - LOCKED for the drag (no mid-drag re-pick)
    void  handleValueDrag(juce::Point<int> pos);
    void  applyInfluence(int ch, int srcStep); // copy srcStep onto every step in the channel

    juce::String stepParamId(int ch, int step) const;
    juce::Rectangle<int> stepRect(int ch, int step) const;
    bool findStepAt(juce::Point<int> pos, int& outCh, int& outStep) const;
    void handleClick(juce::Point<int> pos, bool setDragState);
    bool lastDragState = false;
};

//==============================================================================
// A draggable "Drag MIDI" source — drag starts on mouseDrag, not onClick
class DragMidiSource : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<juce::File()> getMidiFile;

    void paint(juce::Graphics& g) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    bool dragStarted = false;
};

//==============================================================================
// Shared helper: build a popup menu for a MIDI-learnable control and run it.
// Shows the current assignment ("Assigned: ch6 cc44" / "Not assigned").
void showMidiLearnMenu(juce::Component* target, MidiLearnManager& mlm,
                       const juce::String& paramId, int forcedChannel);

//==============================================================================
// Knob with right-click MIDI-learn popup.
class LearnableKnob : public juce::Slider
{
public:
    LearnableKnob(const juce::String& paramId, MidiLearnManager& mlm);
    void mouseDown(const juce::MouseEvent& e) override;
    void paint(juce::Graphics& g) override
    {
        juce::Slider::paint(g);
        if (midiLearn.isLearning() && midiLearn.getLearningParam() == paramId)
        {   // amber wash + ring on the rotary while this control waits to learn a CC
            auto r = getLocalBounds().toFloat();
            float sz = juce::jmin(r.getWidth(), r.getHeight());
            auto sq = juce::Rectangle<float>(r.getX(), r.getY(), r.getWidth(), sz).reduced(1.0f);
            g.setColour(juce::Colour(0x4dffd23b)); g.fillRoundedRectangle(sq, 6.0f);
            g.setColour(juce::Colour(0xffffd23b)); g.drawRoundedRectangle(sq, 6.0f, 2.4f);
        }
    }
    juce::String paramId;
    MidiLearnManager& midiLearn;
    std::function<int()> learnChannelProvider; // returns forced channel, or null
};

//==============================================================================
// Button with right-click MIDI-learn popup.
class LearnableButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;   // inherit text ctor so it works as a value member
    LearnableButton(const juce::String& text, const juce::String& paramId, MidiLearnManager& mlm);
    void mouseDown(const juce::MouseEvent& e) override;
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        juce::TextButton::paintButton(g, over, down);
        if (midiLearn && midiLearn->isLearning() && midiLearn->getLearningParam() == paramId)
        {   // amber ring while this control is waiting to learn a CC
            g.setColour(juce::Colour(0xffffd23b));
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f);
        }
    }
    juce::String paramId;
    MidiLearnManager* midiLearn = nullptr; // set later when default-constructed
    std::function<int()> learnChannelProvider;
};

//==============================================================================
// One editable parameter of a slot (drives the data-driven per-slot knob panels).
struct SlotParam
{
    juce::String label;
    double min = 0.0, max = 1.0;
    std::function<double(const DrumChannel::Slot&)> get;
    std::function<void(DrumChannel::Slot&, double)>  set;
    juce::StringArray choices;   // non-empty => discrete knob (Shape/Type/Material)
    juce::String suffix;         // display suffix ("st", "ms", "Hz", "x"...)
    bool isPct = false;          // show as 0..100%
    juce::String tooltip;        // optional hover help for the knob
    bool resonGated = false;     // SrcOsc: only shown once the slot's resonator (resonAmt) is on
    bool reBake = false;         // Sample: changing this needs a SoundTouch re-bake on drag-end (Stretch)
};
// env (Atk/Hold/Dec[/Sustain][/Vibrato]) + the engine's own params. -1 => empty.
juce::Array<SlotParam> slotParamsFor(int engine);

//==============================================================================
// Visual wave editor for Analog + FM slots: draws the carrier waveform morphing from
// Wave A (left = note start) to Wave B (right = note end). Click the LEFT half to cycle
// Wave A, the RIGHT half to cycle Wave B. In FM mode it ALSO phase-modulates the drawing
// by the slot's Ratio + Depth, so the picture is the actual FM tone and updates live as
// you turn those knobs. Replaces the old "Wave A"/"Wave B" step-knobs.
class WaveMorphDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<DrumChannel::Slot*()> getSlot;   // -> the slot this edits
    std::function<void()> onEdit;                  // after cycling a wave (mark DSP dirty)
    bool fmMode = false;                           // FM: apply phase modulation to the drawing
    bool compact = false;                          // short form (no A/B label strip) when the box is tight
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    static float basicWave(int shape, float ph01);
};

//==============================================================================
// WavetableDisplay - shows the SrcWave engine's current single-cycle waveform (read from the
// real wavetable bank at the slot's Table + Position). Faint neighbour frames behind give the
// "table" feel; the bright wave morphs as you turn Position. Read-only (no click editing).
class WavetableDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<DrumChannel::Slot*()> getSlot;   // -> the slot this shows
    void paint(juce::Graphics& g) override;
    juce::String getTooltip() override
    {
        return "Wavetable: the current single-cycle waveform. Turn Position to scan/morph through the table's "
               "waveforms; pick a different Table for a different set. The faint shapes behind are the neighbouring "
               "frames in the table.";
    }
};

//==============================================================================
// One slot's editor: an engine dropdown + a pool of knobs that reconfigure to the
// chosen engine's parameters, all editing a single DrumChannel::Slot. Three of
// these make up the new SOUND BLEND row (each fully editable, duplicates allowed).
class SlotEditor : public juce::Component
{
public:
    static constexpr int MAXK = 24;            // Synth engine needs the most knobs
    static constexpr int KNOB = 42, GAP = 6;   // bigger, easier-to-grab knobs (slots are wide now)
    static constexpr int PER_ROW = 7;          // wide slots -> one row fits every engine (no wrap)
    static constexpr int MAX_ROW = 7;          // widest single row, used to size the engine boxes
    static int rowWidth(int n) { return n > 0 ? n * KNOB + (n - 1) * GAP : 0; }
    int index = 0;
    int engine = -1;
    std::vector<std::unique_ptr<LearnableKnob>> knobs;
    std::vector<std::unique_ptr<juce::Label>>   labels;
    juce::Array<SlotParam> params;
    std::function<DrumChannel::Slot*()> getSlot;   // -> current channel's slots[index]
    std::function<void()> onEdit;                  // after a knob change (mark DSP dirty)
    std::function<void()> onAudition;              // on knob release: play a TEST hit (gated by the "Auto" toggle)
    std::function<void(int)> onSampleEdit;         // Sample: re-bake (SoundTouch) on drag-end after a reBake param (Stretch)
    bool pendingRebake = false;                    // a reBake param was touched this drag
    WaveMorphDisplay morphView;                     // Analog/FM only: Wave A->B morph (replaces 2 knobs)
    WavetableDisplay waveView;                       // SrcWave only: the current wavetable waveform
    // SrcOsc only: the box is split into ANALOG (wave + Freq fader) / FM (Depth fader + Ratio/Feedback
    // knobs) / PHYSICAL (Reson fader + revealed knob row). Freq, Depth (FM amount) and Reson (resonator
    // amount) are each a horizontal fader leading its section, so each knob group is one clean row.
    std::unique_ptr<LearnableKnob> freqFader, depthFader, resonFader;
    // SrcOsc wave pickers: From / To vertical faders flanking the wave (replace click-to-cycle) + a
    // horizontal Warp fader (phase skew / PWM) in the ANALOG section.
    std::unique_ptr<LearnableKnob> fromFader, toFader, warpFader;

    void init(int idx, MidiLearnManager& mlm, juce::LookAndFeel* knobLNF,
              std::function<DrumChannel::Slot*()> slotFn, std::function<void()> editFn);
    // Per-slot accent colour (Slot 1 = yellow, Slot 2 = pink) - tints this slot's knobs + faders so the
    // group reads as that slot's colour (good contrast on the dark tinted box).
    juce::Colour accent { 0xff35c0ff };
    void setAccent(juce::Colour c)
    {
        accent = c;
        for (auto& k : knobs) { k->setColour(juce::Slider::rotarySliderFillColourId, c);
                                k->setColour(juce::Slider::thumbColourId, c);
                                k->setColour(juce::Slider::trackColourId, c); }   // filled side of the engine FADERS = slot colour
        for (auto* f : { freqFader.get(), depthFader.get(), resonFader.get(), fromFader.get(), warpFader.get() })
            if (f) f->setColour(juce::Slider::trackColourId, c);
    }
    void setEngine(int eng);     // rebuild params + show/hide knobs
    void pushValues();           // slot -> knobs (no notification)
    void place(int boxX, int yTop, int boxW, int boxH); // lay out the visible knobs
    void paint(juce::Graphics& g) override;             // SrcOsc section divider lines + labels
    // How many params are actually shown right now: trailing resonGated params (the
    // Osc resonator's Drive/Material/Tone/Position) are hidden until the slot's Reson is up.
    int  activeParamCount() const;
    void updateKnobVisibility();   // show/hide knobs per activeParamCount()
    void relayoutSelf();           // re-run place() with the last geometry (after a reveal toggles)

private:
    void placeOsc(int boxW);       // SrcOsc sectioned layout (called from place())
    void placeGeneric(int boxW);   // the original wave + wrapped-knob-grid layout
    int lastBoxX = 0, lastYTop = 0, lastBoxW = 0, lastBoxH = 0;   // last place() geometry
    int lastActiveCount = -1;      // detect when the reson reveal changes -> relayout
    // SrcOsc section markers (set in placeOsc, drawn in paint): divider line Ys + label strips.
    bool oscLayout = false; int fmLineY = -1, resLineY = -1;
    juce::Rectangle<int> fmLabelR, resLabelR, oscLabelR, warpLabelR, freqLabelR;
};

//==============================================================================
// A global pattern selector button: shows its number + MIDI channel, highlights
// when it's the current pattern, and is itself MIDI-learnable.
class PatternButton : public juce::Component,
                      public juce::DragAndDropTarget,
                      public juce::SettableTooltipClient
{
public:
    int index = 0;
    bool isCurrent = false;  // the VIEWED/selected pattern (updated by editor)
    bool isPlaying = false;  // the pattern the transport is currently playing
    bool dragOver  = false;  // a pattern is being dragged over this one (drop highlight)
    MidiLearnManager* midiLearn = nullptr;
    std::function<void()> onSelect;
    std::function<void(int srcIndex)> onCopyFrom;   // drop: copy srcIndex's content into this pattern

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    juce::String paramId() const { return "p" + juce::String(index) + "_select"; }

    // DragAndDropTarget: accept another pattern button's drag => copy it here.
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("pat"); }
    void itemDragEnter(const SourceDetails&) override { dragOver = true;  repaint(); }
    void itemDragExit (const SourceDetails&) override { dragOver = false; repaint(); }
    void itemDropped  (const SourceDetails& d) override
    {
        dragOver = false; repaint();
        const int src = d.description.toString().substring(3).getIntValue();
        if (onCopyFrom && src != index) onCopyFrom(src);
    }
};

//==============================================================================
// Interactive A-H-D-S-R envelope editor: drag the breakpoint handles to set
// Attack / Hold / Decay+Sustain / Release. Times use fixed skewed bands (no
// reflow), seconds. onChange fires live while dragging.
class ADSRDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setValues(float a, float h, float d, float s, float r);  // load (no callback)
    void setPlayheads(const float* sec, int n);                   // live voice positions (seconds) -> moving dots
    void setEnabledLook(bool en) { if (en == enabledLook) return; enabledLook = en; repaint(); }  // grey out (samples have no AHDSR)
    void setStrikeRing(bool sr) { if (sr == strikeRing) return; strikeRing = sr; repaint(); }      // Physical: 2-handle Strike/Ring (no Hold)
    void setNa(const juce::String& main, const juce::String& sub) { if (main == naMain && sub == naSub) return; naMain = main; naSub = sub; repaint(); }  // greyed-state message
    std::function<void(float,float,float,float,float)> onChange;  // a,h,d,s,r on drag
    std::function<void()> onDragEnd;                              // released after editing (for auto-audition)

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { const bool ed = drag >= 0; drag = -1; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;

    static constexpr float maxA = 6.0f, maxH = 6.0f, maxD = 6.0f, maxR = 0.05f;  // A/H/D up to 6 s (Release retired from UI)
    static constexpr float maxAStrike = 0.05f;   // Strike/Ring mode: attack (Strike) maxes at 50 ms (a strike is short)
    static constexpr float kMinHold = 14.0f;   // px the Hold handle sits right of Attack (so it's visible at hold=0)
    static constexpr float kSkew = 0.22f;      // <1 => lots of room / fine control at the low (ms) end (very slow start)
private:
    float atk = 0.01f, hld = 0.0f, dcy = 0.1f, sus = 0.0f, rel = 0.06f;
    bool  enabledLook = true;      // false for sample slots (no amp envelope)
    bool  strikeRing  = false;     // Physical engine: show a 2-handle Strike(attack)/Ring(decay) envelope, no Hold
    juce::String naMain = "AMP ENVELOPE (n/a - sample)", naSub = "sample plays full length";  // greyed-state text
    int   drag = -1, hover = -1;   // handle index: 0=A 1=H 2=Decay (to zero), -1=none. Release removed; sustain not edited here.
    static constexpr int kMaxHeads = 8;
    float heads[kMaxHeads] = {}; int numHeads = 0;   // live playhead times (seconds)
    juce::Point<float> playheadXY(float ts) const;   // a time -> point on the drawn curve
    struct Geo { float left, top, bottom, h, xA, xH, xD, xS, xR, susY; };
    Geo geom() const;
    void handlePts(juce::Point<float> out[4]) const;
    int  nearestHandle(juce::Point<float> p) const;
    static float skew(float u)    { return std::pow(juce::jlimit(0.0f, 1.0f, u), kSkew); }
    static float invSkew(float u) { return std::pow(juce::jlimit(0.0f, 1.0f, u), 1.0f / kSkew); }
};

//==============================================================================
// A tiny "1 | 2 | 3" button strip used to pick which sound slot the shared shape
// editors (amp envelope, pitch, voice) currently act on. Click sets onSelect(i).
class SlotSelector : public juce::Component
{
public:
    int sel = 0;                              // index into labels (default 0/1/2 = slots)
    juce::StringArray labels { "1", "2" };   // slots (NUM_SLOTS); EQ target overrides to {"All","1","2"}
    std::function<void(int)> onSelect;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
};

//==============================================================================
// A compact level meter. `level`/`peak` are 0..1 display values (already dB-scaled +
// ballistic-smoothed by the editor timer). Fills green -> amber -> red and draws a peak-hold
// tick. Horizontal (strip) or vertical (master). Display-only, no mouse interaction.
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    float level = 0.0f;     // 0..1 smoothed bar
    float peak  = 0.0f;     // 0..1 peak-hold marker
    bool  horizontal = true;
    void setLevel(float l, float pk) { level = l; peak = pk; repaint(); }
    void paint(juce::Graphics& g) override;
};

//==============================================================================
// Interactive pitch-envelope + voice visual. Pitch (semitones, 0 = the centre line)
// starts at +Amount, holds for the Offset delay, then slides to 0 over Time. Three
// handles: Amount (left edge, drag Y), Offset (the knee, drag X), Time (the end, drag X).
// It ALSO visualises the voice: Unison draws extra faint copies spread by Detune, and
// Vibrato wiggles the held-note tail. onChange(amt,time,off) edits the envelope; the
// voice values are display-only (set by the Unison/Detune/Vibrato knobs).
// 4-dot pitch envelope. X = time as a fraction (0..1) of the sound's length (the whole
// width = 100% of the hit), Y = pitch in semitones. The line is anchored at 0 on both
// edges - (0,0) -> 4 draggable dots -> (1,0) - so pitch starts and ends at normal.
class PitchEnvDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int   NDOT = 4;
    void setDots(const float* pitch, const float* timeFrac);  // NDOT + NDOT from the slot
    void setLengthSec(float s) { s = juce::jmax(0.001f, s); if (s == lenSec) return; lenSec = s; repaint(); }  // sound length
    void setPlayheads(const float* secs, int n);              // moving dots while playing
    void setVoice(int, float, float) {}                        // (legacy no-op; voice has its own knobs)
    std::function<void(const float* pitch, const float* timeFrac)> onChange;
    std::function<void()> onDragEnd;                              // released after editing (for auto-audition)
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;   // double-click empty space -> reset to flat
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { const bool ed = drag >= 0; drag = -1; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;
    void setEnabledLook(bool en) { enabledLook = en; repaint(); }   // grey out for Noise/no-pitch slots

    static constexpr float maxSt = 24.0f;     // +/- 2 octaves (musical pitch-env range, like pro samplers;
                                              // was 48 = 4 oct, which made samples shoot up to ~16x speed)
    static constexpr int   kMaxHeads = 8;
private:
    float p[NDOT] = { 0, 0, 0, 0 }, t[NDOT] = { 0.2f, 0.4f, 0.6f, 0.8f };
    float lenSec = 0.2f;
    float heads[kMaxHeads]; int nHeads = 0;
    int   drag = -1, hover = -1;
    bool  enabledLook = true;
    struct Geo { float left, right, top, bottom, cy, hh; };
    Geo  geom() const;
    float xForT(const Geo& q, float f) const { return q.left + juce::jlimit(0.0f, 1.0f, f) * (q.right - q.left); }
    float yForP(const Geo& q, float st) const { return q.cy - juce::jlimit(-1.0f, 1.0f, st / maxSt) * q.hh; }
    void  buildCurve(juce::Path& path, const Geo& q) const;
    int   nearestHandle(juce::Point<float> pos) const;
};

//==============================================================================
// Interactive VOICE controller for one slot: Unison (count) + Detune (spread) + Vibrato (depth),
// drawn as a cluster of detuned voice lines (count = how many, spread = how far apart) that wobble
// by the vibrato depth. Three draggable handles (like the env editors): Unison (cyan, drag Y),
// Detune (amber, drag Y), Vibrato (pink, drag Y). Greys with a reason when the engine has no pitch
// (Noise) or doesn't support a control - so e.g. Sample shows only Vibrato, Modal shows "(n/a)".
class VoiceModDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kMaxUni = 7;
    void setValues(int unison, float detune, float vibrato, bool centre, int detuneMode);
    void setSupport(bool uniSupported, bool vibSupported, juce::String naReason);
    std::function<void(int unison, float detune, float vibrato, bool centre, int detuneMode)> onChange;
    std::function<void()> onDragEnd;                              // released after editing (for auto-audition)
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { const bool ed = drag >= 0; drag = -1; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;
private:
    int   uni = 1;
    float det = 0.0f, vib = 0.0f;
    bool  centre = false;          // also play the original/undetuned pitch (toggled by double-click on Detune)
    int   mode = 0;                // detune direction: 0 = symmetric (drag right), 1 = up (drag up), 2 = down (drag down)
    bool  uniOn = true, vibOn = true;
    juce::String reason;
    int   drag = -1, hover = -1;   // 0 = Unison, 1 = Detune, 2 = Vibrato
    struct Geo { float left, right, top, bottom, cy, hh, uX, dX, vX;
                 float rangeX, rangeY, dPtX, dPtY; };   // detune handle: range + its mode-aware position
    Geo  geom() const;
    int  nearestHandle(juce::Point<float> p) const;
    void emit() { if (onChange) onChange(uni, det, vib, centre, mode); }
};

//==============================================================================
// Overlays the live spectrum of the inspected channel with the EQ+filter
// response curve (which reshapes as you turn the EQ/HP/LP knobs).
class FrequencyDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int scopeSize = 160;

    // Frequency axis 20 Hz - 20 kHz. skew < 1 packs the low end tighter and gives the
    // high end more room (so the curve doesn't "start too slow").
    static constexpr float kAxisSkew = 0.65f;
    static float normToFreq(float u) {
        u = juce::jlimit(0.0f, 1.0f, u);
        return 20.0f * std::pow(1000.0f, std::pow(u, kAxisSkew));
    }
    static float freqToNorm(float f) {
        float l = std::log10(juce::jlimit(20.0f, 20000.0f, f) / 20.0f) / 3.0f; // 0..1 (pure log)
        return std::pow(juce::jlimit(0.0f, 1.0f, l), 1.0f / kAxisSkew);
    }

    static constexpr float kMaxDb = 18.0f;       // vertical range +/- dB
    // Point the display at a channel's EQ bands (drawn + dragged in place) + formant info.
    void setBands(DrumChannel::EqBand* b, int formantType, float cutoff, float reso, double sr);
    std::function<void()> onEdit;                // after a drag/wheel/toggle -> updateDSP + hash
    std::function<void()> onDragEnd;             // released after editing (for auto-audition)
    void pushSpectrum(const float* mags, int n);
    void decayTick();                            // call on a timer to fade slowly
    void paint(juce::Graphics& g) override;
    void mouseDown   (const juce::MouseEvent& e) override;
    void mouseDrag   (const juce::MouseEvent& e) override;
    void mouseMove   (const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseExit   (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp     (const juce::MouseEvent&) override { const bool ed = drag >= 0; drag = -1; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;

private:
    DrumChannel::EqBand* bands = nullptr;         // -> selected channel's eqBand[5]
    int   fType = 0;
    float fCutoff = 1000.0f, fReso = 0.707f;
    double sampleRate = 44100.0;
    float scope[scopeSize]  = {}; // spectrum outline (peak-hold for consistency)
    bool  hasSpectrum = false;
    int   drag = -1, hover = -1;  // EQ band index being dragged / hovered

    float xForFreq(juce::Rectangle<float> a, float f) const { return a.getX() + freqToNorm(f) * a.getWidth(); }
    float freqForX(juce::Rectangle<float> a, float x) const { return normToFreq((x - a.getX()) / juce::jmax(1.0f, a.getWidth())); }
    float yForDb (juce::Rectangle<float> a, float db) const { return a.getCentreY() - juce::jlimit(-1.0f, 1.0f, db / kMaxDb) * a.getHeight() * 0.5f; }
    float dbForY (juce::Rectangle<float> a, float y)  const { return juce::jlimit(-kMaxDb, kMaxDb, (a.getCentreY() - y) / (a.getHeight() * 0.5f) * kMaxDb); }
    float responseDb(float f) const;              // combined EQ magnitude at f (dB)
    int   nearestBand(juce::Point<float> p) const;
    juce::Point<float> handlePos(juce::Rectangle<float> a, int b) const;
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced(6.0f); }
};

//==============================================================================
// 2D blend pad for the "Sounds" section. One corner per occupied SLOT (not per
// engine type), so two Analog slots get two separate corners ("Analog 1" /
// "Analog 2"). A draggable dot blends the slots: 3 -> triangle (barycentric),
// 2 -> slider, 1 -> 100%. Weights always sum to 1 over the active slots.
class SoundPad : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int NS = 3;     // blend slots (each picks an engine; duplicates allowed)
    SoundPad() { names = { "", "", "" }; }

    bool   active[NS]  = { true, false, false };
    float  weights[NS] = { 1, 0, 0 };
    float  dotX = 0.5f, dotY = 0.5f; // normalised position in the unit square
    bool   layoutB = false;          // legacy A/B corner swap (only meaningful at 4+ corners)
    juce::StringArray names;
    std::function<void()> onChange;  // called after weights change (drag / reflow)

    void setActiveMask(const bool a[NS]); // updates toggles, recentres dot, recomputes
    void setDot(float nx, float ny);     // restore a saved position
    void recompute();                    // weights from dot + current geometry

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

private:
    int  activeCount() const;
    // Normalised [0,1]^2 vertex positions for the active sources, in index order.
    void vertices(juce::Array<juce::Point<float>>& out) const;
    void centroid(float& nx, float& ny) const;
    juce::Point<float> toPixels(juce::Point<float> n) const;
    // Reorder the active-source list for the current A/B layout so different
    // pairs sit on adjacent corners (square: swap top; pentagon: pentagram order).
    void applyLayout(juce::Array<int>& act) const;
};

//==============================================================================
// Cached waveform view of a sample. Peaks are bucketed to the box width so any
// length fits. When selection is enabled you can drag a start/end region.
class WaveformDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int MAXREG = 4;            // up to 4 hand-drawn regions (green / yellow / pink / cyan)
    void setPeaks(const std::vector<float>& mins, const std::vector<float>& maxs)
    { pMin = mins; pMax = maxs; repaint(); }
    void clear() { pMin.clear(); pMax.clear(); repaint(); }
    void setSelectionEnabled(bool en) { if (en != selEnabled) { selEnabled = en; repaint(); } }  // Trim on/off
    void setRegions(int n, const float* lo, const float* hi)
    {
        regN = juce::jlimit(0, MAXREG, n);
        for (int i = 0; i < regN; ++i) { regLo[i] = lo[i]; regHi[i] = hi[i]; }
        repaint();
    }
    void setLength(float secs) { if (secs != lengthSec) { lengthSec = secs; repaint(); } }          // length watermark
    void setReversed(bool r) { if (r != reversed) { reversed = r; repaint(); } }                    // REV badge
    // Called whenever the regions change (drag/clear). The editor writes them to the slot.
    std::function<void(int n, const float* lo, const float* hi)> onRegionsChange;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void emitRegions() { if (onRegionsChange) onRegionsChange(regN, regLo, regHi); }
    std::vector<float> pMin, pMax;
    int   regN = 0;                        // number of drawn regions
    float regLo[MAXREG] = {}, regHi[MAXREG] = {};
    float dragAnchor = 0.0f; int dragIdx = -1;   // the region being dragged out
    bool  selEnabled = false;
    float lengthSec = 0.0f;    // sample length (watermark)
    bool  reversed = false;    // draw a REV badge when the sample plays backwards
};

//==============================================================================
// A sliding on/off switch: green with the knob to the right when on, grey with
// the knob to the left when off.
class ToggleSwitch : public juce::Button
{
public:
    ToggleSwitch() : juce::Button({}) { setClickingTogglesState(true); }
    juce::Colour onColour { 0xff35b56a };   // "on" fill (override for special toggles)
    void paintButton(juce::Graphics& g, bool, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.0f);
        const bool on = getToggleState();
        const float r = b.getHeight() * 0.5f;
        g.setColour(on ? onColour : juce::Colour(0xff4a4a5e));
        g.fillRoundedRectangle(b, r);
        const float d  = b.getHeight() - 4.0f;
        const float kx = on ? b.getRight() - d - 2.0f : b.getX() + 2.0f;
        g.setColour(juce::Colours::white);
        g.fillEllipse(kx, b.getY() + 2.0f, d, d);
    }
};

//==============================================================================
// Look-and-feel for the knob value read-out under each knob.
struct KnobLNF : juce::LookAndFeel_V4
{
    juce::Label* createSliderTextBox(juce::Slider& s) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox(s);
        l->setFont(juce::Font(12.5f));
        l->setJustificationType(juce::Justification::centred);
        return l;
    }
};

// The SOUND BLEND vertical fader, drawn two-tone: PINK on top (Slot 1) / YELLOW on the bottom (Slot 2),
// with a white thumb. Split at the thumb so it reads as a balance.
struct TwoToneFaderLNF : KnobLNF
{
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h,
                          float sliderPos, float /*minPos*/, float /*maxPos*/,
                          juce::Slider::SliderStyle, juce::Slider&) override
    {
        const float cx = x + w * 0.5f, tw = 6.0f;
        juce::Rectangle<float> track(cx - tw * 0.5f, (float) y, tw, (float) h);
        g.setColour(juce::Colour(0xff202038));                    // dark groove behind
        g.fillRoundedRectangle(track, 3.0f);
        // Above the thumb = PINK (Slot 1), below the thumb = YELLOW (Slot 2).
        g.setColour(juce::Colour(0xffff5fa6));
        g.fillRoundedRectangle(track.withBottom(sliderPos), 3.0f);
        g.setColour(juce::Colour(0xffe7c33c));
        g.fillRoundedRectangle(track.withTop(sliderPos), 3.0f);
        // Thumb.
        const float th = 7.0f;
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle((float) x, sliderPos - th * 0.5f, (float) w, th, 2.5f);
        g.setColour(juce::Colour(0xff141422));
        g.drawRoundedRectangle((float) x + 0.5f, sliderPos - th * 0.5f, (float) w - 1.0f, th, 2.5f, 0.8f);
    }
};

// Live STEREO "volume meter" built from the logo's step ramp (display only). Two mirrored ramps that meet
// in the middle: LEFT channel on the left half (tall at the left edge), RIGHT channel on the right half
// (tall at the right edge). Each lights amber->red as that channel's output level rises.
struct LogoStepMeter : juce::Component
{
    float levelL = 0.0f, levelR = 0.0f;   // 0..1 smoothed per-channel level
    LogoStepMeter() { setInterceptsMouseClicks(false, false); }
    void setLevels(float l, float r)
    {
        l = juce::jlimit(0.0f, 1.0f, l); r = juce::jlimit(0.0f, 1.0f, r);
        if (std::abs(l - levelL) > 0.004f || std::abs(r - levelR) > 0.004f) { levelL = l; levelR = r; repaint(); }
    }
    void paint(juce::Graphics& g) override
    {
        const int N = 7;
        auto r = getLocalBounds().toFloat();
        const float mid = r.getCentreX(), halfW = r.getWidth() * 0.5f, gap = 1.5f;
        const float bw = juce::jmax(2.0f, (halfW - gap * (N + 1)) / (float) N);
        auto side = [&](float level, bool rightSide)
        {
            for (int i = 0; i < N; ++i)
            {
                const float frac = (float) (i + 1) / (float) N;              // ascending OUTWARD (tall at the edge)
                const float bh = r.getHeight() * (0.30f + 0.70f * frac);
                const float x = rightSide ? mid + gap + i * (bw + gap)
                                          : mid - gap - (i + 1) * (bw + gap);
                juce::Rectangle<float> bar(x, r.getBottom() - bh, bw, bh);
                const bool lit = level >= (float) i / (float) N;
                juce::Colour c = (i >= N - 1) ? juce::Colour(0xffff2d2d)
                               : juce::Colour(0xffffc24b).interpolatedWith(juce::Colour(0xffff7a1f), frac);
                if (lit) { g.setColour(c); g.fillRoundedRectangle(bar, 1.2f); }     // inside = live volume
                g.setColour(c.withAlpha(0.95f)); g.drawRoundedRectangle(bar, 1.2f, 1.0f);  // outline ALWAYS visible
            }
        };
        side(levelL, false);   // LEFT speaker -> left half
        side(levelR, true);    // RIGHT speaker -> right half
    }
};

// Small bold font for the tiny channel-strip toggle buttons (M / S / Ø / OV)
// so 2-letter labels fit without being cut to "...".
struct TinyButtonLNF : juce::LookAndFeel_V4
{
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(11.5f, juce::Font::bold); }
    int getTextButtonWidthToFitText(juce::TextButton&, int) override { return 0; }
};

// Makes a ComboBox's popup flow into up to 3 columns (and only the 3rd scrolls)
// instead of one tall scrolling list - used for the big sound-mix menu.
struct WideMenuLNF : juce::LookAndFeel_V4
{
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox& box, juce::Label& label) override
    {
        return juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu(box, label)
                 .withMaximumNumColumns(3);
    }
};

// Bigger text in a ComboBox + its popup (used for the sample chooser).
struct BigComboLNF : juce::LookAndFeel_V4
{
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(16.0f); }
    juce::Font getPopupMenuFont() override               { return juce::Font(18.0f); }
    // Make the dropped-down rows tall enough for the bigger font (otherwise the
    // item height is computed from a default font and the text looks cramped).
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                   int standardHeight, int& w, int& h) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize(text, isSeparator, standardHeight, w, h);
        if (! isSeparator) h = juce::jmax(h, 26);
    }
};

// Draws a glyph (play / stop / undo / redo) instead of text. Set the button's property "icon" to pick which.
struct IconButtonLNF : juce::LookAndFeel_V4
{
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override {}   // glyph only, no text
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                              bool over, bool down) override
    {
        juce::LookAndFeel_V4::drawButtonBackground(g, b, bg, over, down);
        const juce::String id = b.getProperties()["icon"].toString();
        auto r = b.getLocalBounds().toFloat();
        const float cx = r.getCentreX(), cy = r.getCentreY(), s = juce::jmin(r.getWidth(), r.getHeight()) * 0.30f;
        g.setColour(juce::Colours::white.withAlpha(b.isEnabled() ? 0.92f : 0.4f));
        if (id == "play")      { juce::Path p; p.addTriangle(cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, cx + s, cy); g.fillPath(p); }
        else if (id == "stop") { g.fillRoundedRectangle(cx - s, cy - s, s * 2.0f, s * 2.0f, 1.0f); }
        else if (id == "undo" || id == "redo")
        {
            const bool redo = (id == "redo");
            juce::Path p;                                  // a ~3/4 arc
            p.addCentredArc(cx, cy + 1.0f, s, s, 0.0f,
                            redo ? 0.9f : -0.9f, redo ? 4.2f : -4.2f, true);
            g.strokePath(p, juce::PathStrokeType(1.8f));
            // arrowhead at the top end of the arc
            const float ax = cx + (redo ? s * 0.55f : -s * 0.55f), ay = cy - s + 1.0f;
            juce::Path h; const float d = 3.2f;
            h.addTriangle(ax, ay - d, ax, ay + d, ax + (redo ? -d * 1.4f : d * 1.4f), ay);
            g.fillPath(h);
        }
    }
};

// A TextButton that draws a clean down-triangle on the right (so it reads as a dropdown, like a ComboBox).
struct DropButtonLNF : juce::LookAndFeel_V4
{
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                              bool over, bool down) override
    {
        juce::LookAndFeel_V4::drawButtonBackground(g, b, bg, over, down);
        auto r = b.getLocalBounds().toFloat();
        const float cx = r.getRight() - 12.0f, cy = r.getCentreY();
        juce::Path tri; tri.addTriangle(cx - 4.0f, cy - 2.5f, cx + 4.0f, cy - 2.5f, cx, cy + 3.0f);  // ▼
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.fillPath(tri);
    }
    // Centre the label in the area LEFT of the ▼ so the text never touches the triangle.
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        juce::Font font(getTextButtonFont(b, b.getHeight()));
        g.setFont(font);
        g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                      .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.5f));
        const int yIndent = juce::jmin(4, b.proportionOfHeight(0.3f));
        const int leftIndent = 6, rightIndent = 20;   // rightIndent clears the ▼
        const int textWidth = b.getWidth() - leftIndent - rightIndent;
        if (textWidth > 0)
            g.drawFittedText(b.getButtonText(), leftIndent, yIndent, textWidth, b.getHeight() - yIndent * 2,
                             juce::Justification::centred, 1);
    }
};

// A tiny square button that draws a "+" (zoom-in) - drawn, not text, so it never
// gets ellipsised to "..." in a small bound.
struct PlusButton : juce::Button
{
    PlusButton() : juce::Button("zoom") {}
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour(down ? 0xff35c0ff : (over ? 0xff3a4673 : 0xff2d3656)));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(juce::Colour(0xff44507a)); g.drawRoundedRectangle(r, 3.0f, 0.8f);
        g.setColour(juce::Colour(down ? 0xff0a0a14 : 0xffcfe0ff));
        // Magnifier glyph (zoom): a lens circle with a short diagonal handle.
        auto c = r.getCentre();
        const float rad = r.getHeight() * 0.24f;
        const juce::Point<float> lens(c.x - rad * 0.4f, c.y - rad * 0.4f);
        g.drawEllipse(lens.x - rad, lens.y - rad, rad * 2.0f, rad * 2.0f, 1.3f);
        const float d = rad * 0.707f;       // lens edge at 45 deg (lower-right)
        g.drawLine(lens.x + d, lens.y + d, lens.x + d + rad * 0.9f, lens.y + d + rad * 0.9f, 1.6f);
    }
};

// Dims a source group whose source is switched OFF (visual only - it lets clicks
// through so the knobs underneath stay usable).
struct FadeOverlay : juce::Component
{
    bool faded = false;
    FadeOverlay() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override
    {
        if (! faded) return;
        g.setColour(juce::Colour(0xa0121020));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
    }
};

// A transparent full-window catcher: a click anywhere on it (i.e. outside the
// floating zoom panel that sits on top of it) closes the zoom. It does NOT paint,
// so the rest of the UI stays fully visible while a group is popped out.
struct ZoomCatcher : juce::Component
{
    std::function<void()> onClick;
    ZoomCatcher() { setInterceptsMouseClicks(true, false); }
    // Dim everything behind the zoom panel so the other boxes (and their dropdowns) don't show through.
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xdc0c0c16)); }
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
};

// The floating panel a zoomed group's controls are re-parented into. Solid
// background + border so the enlarged knobs read clearly over whatever is behind.
struct ZoomPanel : juce::Component
{
    ZoomPanel() { setInterceptsMouseClicks(true, true); }
    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff14141f)); g.fillRoundedRectangle(b, 6.0f);
        g.setColour(juce::Colour(0xff35c0ff)); g.drawRoundedRectangle(b.reduced(1.0f), 6.0f, 1.6f);
    }
};

//==============================================================================
class DrumSequencerEditor;

// Holds all the UI at a fixed "design" resolution; the editor scales it.
class ContentComponent : public juce::Component
{
public:
    ContentComponent(DrumSequencerEditor& o) : owner(o) {}
    void paint(juce::Graphics& g) override;
    void resized() override;
    DrumSequencerEditor& owner;
};

//==============================================================================
class DrumSequencerEditor : public juce::AudioProcessorEditor,
                             public juce::DragAndDropContainer,
                             public juce::Timer,
                             public MidiLearnManager::Listener,
                             public juce::FileBrowserListener,
                             private juce::ScrollBar::Listener
{
public:
    DrumSequencerEditor(DrumSequencerProcessor&);
    ~DrumSequencerEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void midiLearnAssignmentChanged(const juce::String&) override { content.repaint(); }

    // Called by the ContentComponent
    void layoutContent();
    void paintContent(juce::Graphics&);

    // Fixed design WIDTH; the design HEIGHT grows with the number of visible channel rows
    // (contentHeightPx, recomputed by setVisibleChannels). DESIGN_H is the 8-channel default.
    static constexpr int DESIGN_W = 1510;   // widened to fit the Keys toggle next to Drag MIDI
    static constexpr int DESIGN_H = 778;   // detail panel ends at the content (no dead bottom band)

    // How many channel rows the grid shows (4/8/12/16). The engine always has NUM_CHANNELS;
    // this only controls the UI. setVisibleChannels recomputes the layout height + relays out.
    int visibleChannels = 8;
    int contentHeightPx = DESIGN_H;         // design height for the current visibleChannels
    void setVisibleChannels(int n);
    void setNumPatterns(int n);
    int  patShown() const { return juce::jmin(visiblePatterns, PAT_VIEW_CAP); }
    static constexpr int CHAN_VIEW_CAP = 8; // most channel rows shown at once (with the editor open); beyond this, scroll
    // With the editor HIDDEN there's room for all channels, so show up to 16 (no scrollbar needed at 12/16).
    int viewRows() const { return juce::jmin(visibleChannels, detailShown ? CHAN_VIEW_CAP : 16); }
    int firstChannelRow = 0;                // topmost visible channel when scrolled
    juce::ScrollBar channelBar { true };    // vertical scrollbar (left of the channel numbers)
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;

private:
    DrumSequencerProcessor& proc;

    ContentComponent content { *this };

    //-- Group zoom: a "Zoom" button beside each group title lifts ONLY that group's
    //   controls into a floating panel, enlarged; the rest of the UI is untouched.
    //   Click outside (the transparent catcher) or Close to return.
    static constexpr int NUM_ZOOM = 14;    // master + 2 slots + amp/eq + pitch + fx (+ legacy/hidden headers)
    PlusButton zoomBtns[NUM_ZOOM];
    int        zoomBoxH[NUM_ZOOM] = {};    // each box's height (set in layoutContent; used by the zoom popup)
    FadeOverlay srcFade[DrumChannel::NUM_SOURCES];  // dims a source group when its source is off
    juce::TextButton zoomCloseBtn { "Close" };
    ZoomCatcher      zoomCatcher;
    ZoomPanel        zoomPanel;
    juce::Array<juce::Component*> zoomMoved;  // controls currently re-parented into the panel
    bool zoomed = false;
    juce::Rectangle<int> zoomRect;            // design-space rect of the zoomed group
    void zoomToGroup(juce::Rectangle<int> designRect);
    void positionZoomPanel();                 // place/scale the floating panel from zoomRect
    void unzoom();

    //-- Step grid
    StepGridComponent stepGrid;

    //-- Top toolbar
    LearnableButton btnDawSync { "DAW Sync", "global_dawsync", proc.midiLearn };
    LearnableButton btnPlay    { "Play",     "global_play",    proc.midiLearn };
    LearnableButton btnStop    { "Stop",     "global_stop",    proc.midiLearn };
    juce::Label     lblBpm     { {}, "BPM:" };
    juce::Slider    sliderBpm;
    juce::Label     lblSwing   { {}, "Swing:" };
    juce::Slider    sliderSwing;
    // Bar-length calculator: editable X/Y time signature + computed bar seconds.
    juce::Label     lblBarPre  { {}, "Bar" };
    juce::Label     barSigX    { {}, "4" };
    juce::Label     lblBarSlash{ {}, "/" };
    juce::Label     barSigY    { {}, "4" };
    juce::Label     lblBarResult;
    int  barTimeSigX = 4, barTimeSigY = 4;
    void updateBarLength();
    juce::Label     titleLabel { {}, "DavulSEQ" };
    std::unique_ptr<juce::Drawable> logoDrawable;   // top-left brand logo (parsed from embedded SVG)

    //-- Sound browser: a FileBrowserComponent window for browsing/loading samples into a slot.
    juce::WildcardFileFilter sampleFilter { "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg;*.WAV;*.AIFF", "*", "Audio files" };
    std::unique_ptr<juce::DialogWindow> browserWin;
    int browseTargetBox = 0;
    void openSoundBrowser(int box);
    // FileBrowserListener:
    void selectionChanged() override {}
    void fileClicked(const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked(const juce::File& f) override;
    void browserRootChanged(const juce::File&) override {}
    DragMidiSource  dragMidi;

    // On-screen MIDI-in monitor (diagnostic: shows the last CC + flashes on input).
    juce::Label  lblMidiIn;
    uint32_t     lastMidiInSeen = 0;
    int          midiFlash = 0;

    // Undo / redo of whole-instrument state (snapshot stack, per-action). Each entry
    // carries the editor's preset-label state too, so undoing a preset switch also
    // restores the previous preset name (not just the underlying parameters).
    struct UndoEntry
    {
        juce::MemoryBlock state;
        juce::String      presetName;
        juce::int64       presetBaselineHash = 0;
        bool              presetModified = false;
    };
    juce::TextButton btnUndo { "Undo" }, btnRedo { "Redo" };
    std::vector<UndoEntry> undoStack, redoStack;
    static constexpr int kUndoMax = 24;
    juce::int64 lastUndoHash = 0;
    int  undoStableTicks = 0;
    bool undoDirty = false;
    bool applyingUndo = false;
    void pushUndoSnapshot();
    void applyUndoState(const UndoEntry& e);
    void doUndo();
    void doRedo();
    void updateUndoRedoEnabled();

    //-- Presets
    juce::Label    lblPreset { {}, "Preset:" };
    juce::ComboBox comboPreset;
    juce::Array<juce::File> presetFiles;

    //-- MIDI menu (top bar; replaces the old "Clear MIDI" button). Clear MIDI-learn + save/load
    //   the current pattern's note grid as a *.basamakpattern file.
    juce::ComboBox comboMidi;
    juce::Array<juce::File> midiPatternFiles;
    juce::File getMidiPatternsFolder();
    void rebuildMidiMenu();
    void handleMidiMenuChange();
    void saveMidiPattern(const juce::File& file);
    void loadMidiPattern(const juce::File& file);

    //-- Pattern row + per-pattern play options
    juce::Label    lblPatterns { {}, "Patterns" };
    PatternButton  patternBtns[Sequencer::NUM_PATTERNS];
    juce::TextButton patModeBtn;   // opens the Loop/Stop/Go-to menu; shows a summary
    juce::TextButton btnFollow { "Follow" };   // global toggle: view follows the playing pattern (proc.followPlayback)
    juce::TextButton btnClearPat { "Clear" };  // wipe the current pattern's steps/values back to default
    juce::ComboBox   comboVisChannels;         // (legacy, hidden) replaced by the 8/16 buttons
    juce::ComboBox   comboNumPat;              // (legacy, hidden) replaced by the 16/32 buttons
    juce::TextButton btnCh8 { "8" }, btnCh16 { "16" };     // channel-count toggle (pattern row, by the loop dropdown; "Ch")
    juce::TextButton btnPat16 { "16" }, btnPat32 { "32" }; // pattern-count toggle (pattern row, by the loop dropdown; "Pat")
    juce::Label      lblChannels { {}, "Ch" }, lblNumPat { {}, "Pat" };   // compact captions (moved to the pattern row)
    juce::TextButton btnTooltips { "Tooltips" };  // top bar: global hover-tooltips ON/OFF (default ON)
    bool             tooltipsOn = true;
    void refreshCountButtons();
    void applyTooltipsSetting();   // turn the TooltipWindow on/off + colour the button
    int   visiblePatterns = 16;                // patterns shown/used; >16 scrolls horizontally
    int   firstPatternCol = 0;                 // scroll offset for the pattern buttons
    juce::ScrollBar  patternBar { false };     // horizontal scrollbar under the pattern row (when >16 patterns)
    static constexpr int PAT_VIEW_CAP = 16;    // pattern buttons visible at once before scrolling
    bool  lastPlayingState = false;            // timer edge-detection for the playing marker
    int   lastPlayPattern = -1;
    void  refreshFollowButton();
    juce::TextButton btnKeys { "Keys" };       // global toggle: MIDI keyboard plays channel sounds (proc.keysMode)
    void  refreshKeysButton();
    juce::TextButton btnAudition { "Auto" };   // global toggle: knob edits auto-play a TEST hit (proc.auditionOnEdit)
    juce::TextButton btnToggleDetail { "HIDE SOUND EDITOR" };   // collapse/expand the sound-editing panel
    bool detailShown = true;                   // when false, only the sequencer is shown (window shrinks)
    void  refreshAuditionButton();
    // Step-grid edit-mode radio buttons (none selected = normal on/off steps).
    LearnableButton btnModeVel { "Vel/Len" }, btnModePitch { "Pitch" }, btnModeProb { "Loop" }, btnModeRoll { "Roll" }, btnModePan { "Pan" };
    juce::Label      lblEditMode;
    void setStepEditMode(int mode);   // 0 normal, 1 vel, 2 pitch, 3 prob
    juce::Slider     sliderPatN;   // repeat count N
    juce::Label      lblLoopCount { {}, "Loop count" };  // caption above sliderPatN

    // A channel's number button that doubles as a drag-copy handle: drag it onto another
    // channel's number to copy this channel's whole sound + steps there. Stays a TextButton
    // (click selects, colour shows routing) - just adds drag-out + drop-in.
    struct NumDragButton : public juce::TextButton, public juce::DragAndDropTarget
    {
        int chIndex = 0;
        bool dragOver = false;
        std::function<void(int srcIndex)> onCopyFrom;
        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (e.getDistanceFromDragStart() < 6) return;
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
                if (! c->isDragAndDropActive())
                    c->startDragging("ch" + juce::String(chIndex), this);
        }
        bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("ch"); }
        void itemDragEnter(const SourceDetails&) override { dragOver = true;  repaint(); }
        void itemDragExit (const SourceDetails&) override { dragOver = false; repaint(); }
        void itemDropped  (const SourceDetails& d) override
        {
            dragOver = false; repaint();
            const int src = d.description.toString().substring(2).getIntValue();
            if (onCopyFrom && src != chIndex) onCopyFrom(src);
        }
        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat();
            juce::Colour bg = findColour(getToggleState() ? juce::TextButton::buttonOnColourId
                                                          : juce::TextButton::buttonColourId);
            if (down) bg = bg.brighter(0.2f); else if (over) bg = bg.brighter(0.1f);
            g.setColour(bg); g.fillRoundedRectangle(r.reduced(0.5f), 3.0f);
            g.setColour(findColour(juce::TextButton::textColourOffId));
            g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawFittedText(getButtonText(), getLocalBounds().reduced(1), juce::Justification::centred, 1);  // scales to fit (no "...")
            if (dragOver) { g.setColour(juce::Colour(0xff35d07a));
                            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 2.0f); }
        }
    };

    //-- Channel strips (left column)
    struct ChannelStrip
    {
        NumDragButton numBtn;
        std::unique_ptr<LearnableButton> btnMute;
        std::unique_ptr<LearnableButton> btnSolo;
        juce::ComboBox   comboSound;     // the "Sound Bank" selector
        juce::TextButton btnTest { "TEST" };
        LearnableButton btnPoly { "OV" }; // overlap / polyphony (MIDI-learnable)
        LearnableButton btnInfluence { "I" }; // arm step-influence for this channel (MIDI-learnable)
        juce::ComboBox   comboSteps;
    };
    ChannelStrip strips[Sequencer::NUM_CHANNELS];

    // Level meters: one horizontal bar per channel strip + a stereo (L/R) master meter. The
    // ballistics (dB scaling, fast attack / slow release, peak-hold) live in timerCallback.
    LevelMeter stripMeter[Sequencer::NUM_CHANNELS];
    LevelMeter masterMeter[2];                 // [0]=L, [1]=R (vertical)
    float meterVal[Sequencer::NUM_CHANNELS] = {};
    float meterPk [Sequencer::NUM_CHANNELS] = {};
    int   meterHold[Sequencer::NUM_CHANNELS] = {};
    float mMeterVal[2] = {}, mMeterPk[2] = {};
    int   mMeterHold[2] = {};

    //-- Detail panel (bottom, selected channel)
    int selectedChannel = 0;

    juce::Label lblSelected;

    // Per-source AHD amplitude envelopes (Sample, Noise, Analog/Osc, FM, Physical).
    LearnableKnob knobSrcAtk[5]  { { "p0_ch0_atk0", proc.midiLearn }, { "p0_ch0_atk1", proc.midiLearn },
                                   { "p0_ch0_atk2", proc.midiLearn }, { "p0_ch0_atk3", proc.midiLearn },
                                   { "p0_ch0_atk4", proc.midiLearn } };
    LearnableKnob knobSrcHold[5] { { "p0_ch0_hld0", proc.midiLearn }, { "p0_ch0_hld1", proc.midiLearn },
                                   { "p0_ch0_hld2", proc.midiLearn }, { "p0_ch0_hld3", proc.midiLearn },
                                   { "p0_ch0_hld4", proc.midiLearn } };
    LearnableKnob knobSrcDec[5]  { { "p0_ch0_dec0", proc.midiLearn }, { "p0_ch0_dec1", proc.midiLearn },
                                   { "p0_ch0_dec2", proc.midiLearn }, { "p0_ch0_dec3", proc.midiLearn },
                                   { "p0_ch0_dec4", proc.midiLearn } };
    juce::Label   lblSrcAtk[5], lblSrcHold[5], lblSrcDec[5];
    LearnableKnob knobPitch   { "p0_ch0_pitch",    proc.midiLearn };
    LearnableKnob knobVolume  { "p0_ch0_volume",   proc.midiLearn };
    LearnableKnob knobPan     { "p0_ch0_pan",      proc.midiLearn };
    LearnableKnob knobSlices  { "p0_ch0_slices",   proc.midiLearn };   // sample slicing (1 = off)
    juce::Label   lblSlices;
    LearnableKnob knobStretch { "p0_ch0_stretch",  proc.midiLearn };   // time-stretch (needs SoundTouch)
    juce::Label   lblStretch;
    LearnableKnob knobCutoff  { "p0_ch0_filterCutoff", proc.midiLearn };
    LearnableKnob knobReso    { "p0_ch0_filterReso",   proc.midiLearn };
    LearnableKnob knobEnvAmt  { "p0_ch0_filterEnvAmt", proc.midiLearn };
    LearnableKnob knobDrive   { "p0_ch0_drive",        proc.midiLearn };
    juce::ComboBox comboFilterType, comboDriveType;


    juce::ComboBox comboOutput;      // per-channel routing (Main / Out 1..N / MIDI Out)
    juce::Label    lblOutput;
    juce::ComboBox comboMidiNote;    // the MIDI note this channel sends in MIDI Out mode
    juce::Label    lblMidiNote;
    static constexpr int kMidiOutId = DrumSequencerProcessor::NUM_AUX_OUTS + 2;  // combo id for "MIDI Out"
    juce::TextButton btnRoute { "Routing" };   // top-bar: open the per-channel routing overview
    void refreshRouting();           // recolour the channel strips + Route button by routing
    LearnableKnob knobReverb  { "p0_ch0_reverb",  proc.midiLearn };
    LearnableKnob knobDelay   { "p0_ch0_delay",   proc.midiLearn };

    LearnableKnob knobReverbRoom { "global_reverbRoom", proc.midiLearn };
    LearnableKnob knobDelayTime  { "global_delayTime",  proc.midiLearn };
    LearnableKnob knobDelayFB    { "global_delayFB",    proc.midiLearn };

    //-- Sample pitch envelope
    LearnableKnob knobPEnvAmt  { "p0_ch0_pitchEnvAmt",  proc.midiLearn };
    LearnableKnob knobPEnvTime { "p0_ch0_pitchEnvTime", proc.midiLearn };
    juce::Label   lblPEnvAmt, lblPEnvTime, hdrPitchEnv;

    //-- "Sounds" section: 4 source toggle-switches + 2D blend pad + per-source knobs
    juce::Label      hdrSounds, lblPadHint;
    SoundPad         soundPad;
    juce::TextButton btnPadLayout { "A" };   // 4-source blend pad A/B corner layout
    ToggleSwitch     srcSwitch[5];     // Sample / Noise / Osc / FM / Physical on-off (legacy, hidden)
    juce::Label      lblSrc[5];
    // Channel A-H-D-S-R envelope: an interactive visual in the SOUND BLEND box that
    // writes to the slots chosen by envTargetCombo (all / 1&2 / 2&3 / 1&3).
    // Shared per-slot "shape" editors. One selected slot (envEditMode, per channel) drives
    // the amp envelope, the pitch envelope, and the Unison/Detune/Vibrato knobs.
    ADSRDisplay      envEditor;
    PitchEnvDisplay  pitchEditor;
    VoiceModDisplay  voiceMod;                    // unison/detune/vibrato visual (replaces the 3 knobs)
    SlotSelector     slotSelAmp, slotSelPitch, slotSelFx;   // 1/2 slot pickers (synced via setShapeSlot)
    SlotSelector     slotSelVoice;                          // 1/2 picker under UNISON/DETUNE/VIBRATO (synced with slotSelPitch)
    juce::Label      hdrBlend2, hdrAmpEnv, hdrEqBox, hdrVoice;   // box/section titles (row 1)
    // === PER-SLOT EQ (begin) - target picker: 0 = All (channel EQ), 1/2/3 = that slot's EQ ===
    SlotSelector     slotSelEq;
    int              eqEditTarget = 0;
    void             refreshEqTarget();          // point freqDisplay at the chosen EQ
    // === PER-SLOT EQ (end) ===
    juce::Label      lblShapeSlot, lblPitchSlot;
    int   envTargetSlot() const;               // 0/1/2 (the shared selected slot)
    void  setShapeSlot(int s);                 // change the selected slot + reload all shape editors
    void  loadEnvIntoEditor();                 // selected slot -> amp envelope display
    void  loadPitchAndVoice();                 // selected slot -> pitch visual + uni/det/vib knobs
    float pitchEnvLenSec(int slotIdx);         // sound length for the pitch X-axis (sample-aware)
    void  applyEnvToTargets(float a, float h, float d, float s, float r);  // amp env -> selected slot
    void  applyPitch(float amt, float time, float off);                   // pitch -> selected slot (per-engine field)
    // 3-slot model: each box picks its engine from a dropdown (the same engine may
    // appear in more than one box). boxEngine[i] = the Source shown in box i (-1 = none).
    juce::ComboBox   slotCombo[DrumChannel::NUM_SLOTS];
    SlotEditor       slotEd[DrumChannel::NUM_SLOTS];   // per-box knob panels (duplicates allowed)
    int              boxEngine[DrumChannel::NUM_SLOTS];   // init to -1 in setupComponents
    LearnableKnob    blendFader { "ui_blend", proc.midiLearn };   // horizontal slot-1<->slot-2 blend (replaces the pad)
    void  syncBoxesFromSrcOn();        // fill boxEngine[] from the channel's slots
    void  onSlotEngineChange(int box); // dropdown handler
    void  rebuildSlotMenus();          // (re)build each slot dropdown incl. the Sample submenu
    // Mirror the channel's slots onto the blend pad (one corner per occupied slot,
    // engines numbered when repeated). recenter=true rebalances; false restores the dot.
    void  syncPadFromSlots(bool recenter);
    juce::ComboBox   comboSampleSel;   // sample chooser (moved here)
    juce::Label      lblSampleSel;
    // Per-slot sample editors: each Sample slot shows its OWN waveform + trim + reverse + length.
    WaveformDisplay  waveform[DrumChannel::NUM_SLOTS];
    ToggleSwitch     swUseRegion[DrumChannel::NUM_SLOTS];
    juce::Label      lblUseRegion[DrumChannel::NUM_SLOTS], lblSampleLen[DrumChannel::NUM_SLOTS];
    ToggleSwitch     swSampleReverse[DrumChannel::NUM_SLOTS];
    juce::Label      lblSampleReverse[DrumChannel::NUM_SLOTS];
    LearnableKnob    knobSpeed { "p0_ch0_speed", proc.midiLearn };
    juce::Label      lblSpeed;
    // Sample source: pitch offset + reverse (pitch/env/time reuse knobPitch/PEnvAmt/PEnvTime)
    LearnableKnob    knobSmpPOff { "p0_ch0_smpPOff", proc.midiLearn };
    juce::Label      lblSmpPOff;
    // Source interaction: Ring (multiply), Warp (cross-pitch FM), Morph (cross-filter).
    LearnableKnob    knobBloom  { "p0_ch0_bloom",  proc.midiLearn };
    LearnableKnob    knobDrift  { "p0_ch0_drift",  proc.midiLearn };
    LearnableKnob    knobSpread { "p0_ch0_spread", proc.midiLearn };
    LearnableKnob    knobPunch  { "p0_ch0_punch",  proc.midiLearn };
    LearnableKnob    knobGlue   { "p0_ch0_glue",   proc.midiLearn };
    juce::Label      lblBloom, lblDrift, lblSpread, lblPunch, lblGlue, lblBlendTitle, lblBlendBot;
    juce::Label      hdrSamplerG;     // "SAMPLE" group header (row 1)
    juce::TextButton btnSaveMix { "SAVE TO SOUND BANK" };
    void cacheWaveform(int channel);
    void updateSampleLengthLabel();
    LearnableKnob  knobLayOscShape { "p0_ch0_layOscShape", proc.midiLearn };
    LearnableKnob  knobLaySineFreq { "p0_ch0_laySineFreq", proc.midiLearn };
    LearnableKnob  knobLaySinePEA  { "p0_ch0_laySinePEA",  proc.midiLearn };
    LearnableKnob  knobLaySinePET  { "p0_ch0_laySinePET",  proc.midiLearn };
    LearnableKnob  knobLaySinePOff { "p0_ch0_laySinePOff", proc.midiLearn };
    LearnableKnob  knobOscUnison  { "p0_ch0_oscUnison", proc.midiLearn };
    LearnableKnob  knobOscDetune  { "p0_ch0_oscDetune", proc.midiLearn };
    LearnableKnob  knobOscSustain { "p0_ch0_oscSustain", proc.midiLearn };
    LearnableKnob  knobOscVib     { "p0_ch0_oscVib", proc.midiLearn };
    juce::Label    lblOscUnison, lblOscDetune, lblOscSustain, lblOscVib;
    LearnableKnob  knobLayNoiseType { "p0_ch0_layNoiseType", proc.midiLearn };
    juce::Label    lblNoiseType;
    LearnableKnob  knobLayNoiseCtr { "p0_ch0_layNoiseCtr", proc.midiLearn };
    LearnableKnob  knobLayNoiseWid { "p0_ch0_layNoiseWid", proc.midiLearn };
    LearnableKnob  knobNoiseSus    { "p0_ch0_noiseSus",    proc.midiLearn };
    juce::Label    lblNoiseSus;
    LearnableKnob  knobFmPitch     { "p0_ch0_fmPitch",     proc.midiLearn };
    LearnableKnob  knobFmSpread    { "p0_ch0_fmSpread",    proc.midiLearn };
    LearnableKnob  knobFmDepth     { "p0_ch0_fmDepth",     proc.midiLearn };
    LearnableKnob  knobFmPEnv      { "p0_ch0_fmPEnv",      proc.midiLearn };
    LearnableKnob  knobFmPTime     { "p0_ch0_fmPTime",     proc.midiLearn };
    LearnableKnob  knobFmPOff      { "p0_ch0_fmPOff",      proc.midiLearn };
    LearnableKnob  knobFmFeedback  { "p0_ch0_fmFeedback",  proc.midiLearn };
    LearnableKnob  knobFmSub       { "p0_ch0_fmSub",       proc.midiLearn };
    LearnableKnob  knobFmSustain   { "p0_ch0_fmSustain",   proc.midiLearn };
    juce::Label    lblFmFeedback, lblFmSub, lblFmSustain;
    juce::Label    lblLayOscShape, lblLaySineFreq, lblLaySinePEA, lblLaySinePET, lblLaySinePOff;
    juce::Label    lblLayNoiseTypeK, lblLayNoiseCtr, lblLayNoiseWid;
    juce::Label    lblFmPitch, lblFmSpread, lblFmDepth, lblFmPEnv, lblFmPTime, lblFmPOff;
    // Physical (Karplus-Strong) source
    LearnableKnob  knobPhysFreq { "p0_ch0_physFreq", proc.midiLearn };
    LearnableKnob  knobPhysTone { "p0_ch0_physTone", proc.midiLearn };
    LearnableKnob  knobPhysMat  { "p0_ch0_physMat",  proc.midiLearn };
    LearnableKnob  knobPhysPEnv { "p0_ch0_physPEnv", proc.midiLearn };
    LearnableKnob  knobPhysPTime{ "p0_ch0_physPTime",proc.midiLearn };
    LearnableKnob  knobPhysPOff { "p0_ch0_physPOff", proc.midiLearn };
    LearnableKnob  knobPhysPos  { "p0_ch0_physPos",  proc.midiLearn };
    LearnableKnob  knobPhysSus  { "p0_ch0_physSus",  proc.midiLearn };
    LearnableKnob  knobPhysVib  { "p0_ch0_physVib",  proc.midiLearn };
    juce::Label    lblPhysFreq, lblPhysTone, lblPhysMat, lblPhysPEnv, lblPhysPTime, lblPhysPOff, lblPhysPos, lblPhysSus, lblPhysVib;
    juce::Label    hdrOscG, hdrNoiseG, hdrFmG, hdrPhysG;
    juce::Label    hdrPitch;          // outline + header for the PITCH / VOICE box (row 1)
    void updateSoundsVisibility();
    void onSoundToggle();              // a source toggle was clicked
    void saveSoundMix();

    //-- Master FX (Reverb + Delay sub-groups) + Master Output
    juce::Label      hdrReverb, hdrDelayG, hdrMasterFX, hdrMasterOut;
    LearnableKnob    knobReverbDecay { "global_reverbDecay", proc.midiLearn };
    LearnableKnob    knobReverbWet   { "global_reverbWet",   proc.midiLearn };
    LearnableKnob    knobReverbPre   { "global_reverbPre",   proc.midiLearn };
    LearnableKnob    knobReverbWidth { "global_reverbWidth", proc.midiLearn };
    juce::Label      lblRevWet, lblRevPre, lblRevWidth;
    ToggleSwitch     swDelaySync;
    juce::Label      lblDelaySync;
    ToggleSwitch     swDelayPingPong;
    juce::Label      lblDelayPingPong;
    LearnableKnob    knobMasterVol   { "global_masterVol",   proc.midiLearn };
    LearnableKnob    knobMasterPan   { "global_masterPan",   proc.midiLearn };
    LearnableKnob    knobMasterLimit { "global_masterLimit", proc.midiLearn };
    LearnableKnob    knobMasterGlue  { "global_masterGlue",  proc.midiLearn };
    ToggleSwitch     swMasterMono;
    juce::Label      lblRevDecay, lblMasterVol, lblMasterPan, lblMasterLimit, lblMasterMono, lblMasterGlue;

    // (EQ knobs removed - the EQ is drawn/dragged on freqDisplay now.)
    juce::Label lblPit,lblVol,lblPan;
    juce::Label lblCutoff,lblReso,lblEnvAmt,lblDrive,lblRev,lblDel;
    juce::Label lblFiltType,lblDrvType;
    juce::Label lblRevRoom,lblDelTime,lblDelFB;
    juce::Label hdrEq, hdrFilter, hdrDrive, hdrChan, hdrSend, hdrMaster;

    KnobLNF knobLNF;                       // value read-out under each knob
    TwoToneFaderLNF blendLNF;              // pink/yellow two-tone SOUND BLEND fader
    TinyButtonLNF tinyBtnLNF;              // small font for M/S/Ø/OV strip buttons
    WideMenuLNF wideMenuLNF;               // 3-column popup for the sound-mix menu
    BigComboLNF bigComboLNF;               // larger font for the sample chooser
    LogoStepMeter logoMeter;              // live master-volume meter built into the logo step ramp
    juce::HyperlinkButton verLink;        // clickable version next to the logo -> opens the Releases page
    DropButtonLNF dropBtnLNF;             // down-triangle for the play-mode + routing "dropdown" buttons
    IconButtonLNF iconBtnLNF;             // play / stop / undo / redo glyphs
    std::vector<LearnableKnob*> allKnobs;  // for clean LNF teardown
    juce::TooltipWindow tooltipWindow { this, 1000 }; // shows tooltips after ~1s hover

    //-- Visuals
    FrequencyDisplay freqDisplay;
    juce::dsp::FFT   fft { SpectrumTap::fftOrder };
    juce::dsp::WindowingFunction<float> fftWindow
        { (size_t) SpectrumTap::fftSize, juce::dsp::WindowingFunction<float>::hann };
    float fftBuf[2 * SpectrumTap::fftSize] = {};
    float scopeData[FrequencyDisplay::scopeSize] = {};
    void updateVisuals();

    bool ignoreKnobCallbacks = false;
    std::unique_ptr<juce::FileChooser> fileChooser;

    //-- Samples & sounds
    juce::Array<juce::File> sampleFiles;
    juce::StringArray       categoryList; // built-in categories (alphabetical)
    static juce::File getSamplesFolder();
    void rescanSamples();

    juce::Array<DrumSoundGenerator::Type> sortedVariants(const juce::String& category) const;
    static juce::String soundDisplayName(DrumSoundGenerator::Type t);
    // Sample chooser combo (in the Sounds section, for the selected channel)
    void rebuildSampleMenu();
    void addFolderToMenu(juce::PopupMenu& menu, const juce::File& folder);
    void refreshSampleSel();
    void handleSampleSelChange();

    //-- Sound mixes (per-channel full presets)
    juce::Array<juce::File> soundMixFiles;
    static juce::File getSoundMixFolder();
    void rescanSoundMixes();
    void addSoundFolderToMenu(juce::PopupMenu& menu, const juce::File& folder);   // Sound Bank subfolders -> submenus
    void rebuildSoundMixMenu(int channel);   // populate strips[ch].comboSound with mix names
    void handleSoundMixChange(int channel);  // load a saved mix onto that channel
    void initChannelMix(int channel);        // reset channel to a clean default mix
    void resetChannelToDefault(DrumChannel& c, int channel); // param reset (no UI/steps)
    void loadSoundMix(int channel, const juce::File& file);
    void writeChannelMix(juce::ValueTree& t, const DrumChannel& ch) const;
    void readChannelMix(const juce::ValueTree& t, DrumChannel& ch, juce::String& missingSample) const;

    void setupComponents();
    void setupKnob(LearnableKnob& knob, juce::Label& lbl, const juce::String& txt,
                   double min, double max, double def, double skew = 1.0,
                   std::function<juce::String(double)> fmt = {});
    void setupGroupHeader(juce::Label& lbl, const char* txt);

    void selectChannel(int ch);
    void selectPattern(int p);
    void copyPatternContent(int src, int dst);   // duplicate src's steps + per-pattern settings into dst
    int  currentPattern() const { return proc.sequencer.currentPattern; }

    //-- Presets
    static juce::File getPresetsFolder();
    void rebuildPresetMenu();
    void handlePresetChange();
    void initPreset();             // clean default kit (120 BPM, 4/4, 8 steps, no steps)
    void syncAfterStateChange();   // push BPM/time-sig to the UI + full refresh
    void refreshPatternOptions();
    void askLoopCount(const juce::String& title, int defVal, std::function<void(int)> onResult);  // typed loop-count dialog
    void fullRefresh();

    //-- Sound-mix / preset "modified (*)" tracking + per-pattern dropdown text
    void        updateStripMixLabel(int ch);    // show this pattern/channel's mix name (+ * if edited)
    void        updatePresetLabel();             // show the loaded preset's name (+ * if edited)
    juce::int64 channelSoundHash(const DrumChannel&) const;
    juce::int64 stateHash() const;               // whole-instrument hash (all patterns + master)
    void        rebaselinePreset(const juce::String& name); // mark current state as the saved baseline
    juce::int64 presetBaselineHash = 0;
    bool        presetModified = false;
    juce::String presetName;

    // Track MIDI-learn state so the timer can repaint the highlighted control
    // when learning starts / moves / finishes.
    bool         lastLearnActive = false;
    juce::String lastLearnParam;
    juce::String stripMixShown[Sequencer::NUM_CHANNELS]; // last text drawn (avoid per-tick repaint)
    juce::String presetShown;

    void updateKnobParamIds();
    void updateStripParamIds();
    void refreshDetailPanel();
    void refreshChannelStrips();
    void refreshPatternButtons();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSequencerEditor)
};
