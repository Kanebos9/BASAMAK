#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// Draws the 8×N step grid. Steps fill the component's actual width, so the
// grid stretches with the window and fewer steps means wider buttons.
// Each step also shows its MIDI assignment ("ch6cc44" / "no midi").
class StepGridComponent : public juce::Component, public juce::SettableTooltipClient
{
public:
    // Step edit mode: 0 = on/off, 1 = Velocity, 2 = Pitch, 3 = Probability(Loop), 4 = Roll, 5 = Pan, 6 = Length.
    enum EditMode { ModeSteps = 0, ModeVel, ModePitch, ModeProb, ModeRoll, ModePan, ModeLen, ModeNudge, ModeModA, ModeModB };
    int editMode = ModeSteps;
    bool influenceArmed[Sequencer::NUM_CHANNELS] = {};   // per channel: next touched step propagates to all

    std::function<void(int ch, int step)> onStepClicked;
    std::function<void(int ch)>           onChannelSelected;
    std::function<int()>                  getLearnChannel; // current pattern's channel
    // Called when a value (velocity/pitch/probability) is edited in a value mode.
    std::function<void(int ch, int step, int mode, float value)> onStepValueChanged;
    // Called when the per-step LOOP condition (Prob mode) is edited: cycle length + enabled-loop bitmask.
    std::function<void(int ch, int step, int len, int mask)> onStepCondChanged;
    // Called when a step's 303-SLIDE flag is toggled (bottom strip of a cell in Pitch mode).
    std::function<void(int ch, int step, bool on)> onStepSlideChanged;
    // Called when a step's MERGE flag is toggled (cmd/shift+click; a merged step continues the previous note).
    std::function<void(int ch, int step, bool on)> onStepMergeChanged;
    // PIANO ROLL: any note-list edit pushes the grid's WHOLE mirror list back to the channel.
    std::function<void(int ch, const DrumChannel::DrawNote*, int count)> onDrawNotesChanged;
    std::function<void(int ch, float vel, float pan)> onDrawVelPan;   // whole-channel Pan (+ default Vel) in piano-roll mode
    std::function<void(int ch)> onDrawModeMaybeChanged;               // ch's roll-vs-step may have changed (fade buttons)
    std::function<void()> onGridDivEdit;   // clicking the snap-grid header: type a value (1-64, 0 = off)
    std::function<void()> onQuantizeEdit;  // clicking the Quantize header: type 1/N, snap note starts once
    std::function<void(int ch, float cents)> onDrawTuneChanged;   // the roll's TUNE fader (-50..+50 cents)
    float drawTune[Sequencer::NUM_CHANNELS] = {};                 // mirror of each channel's drawTuneCents
    std::function<void(int semi)> onRollPreview;   // AUDITION while drawing/moving a note (hear the pitch live)
    std::function<void()> onRollPreviewEnd;        // gesture ended -> release the preview note
    // GHOST LINES: asks the editor which pitches slot 'slot' ACTUALLY sounds for a drawn note at
    // 'semi' (chord/scale voicing + slot-2 transpose). Fills out[] (max 8), returns the count (0 = slot silent).
    std::function<int(int ch, int slot, int semi, int* out)> getSlotVoicing;
    int  gridDiv() const;
    void setGridDiv(int n);
    static constexpr int DRAW_ITEM_ID = 100;   // the "Piano Roll" item id in the step-count dropdown
    static constexpr int GRP_MAX = 8;          // merged-group view: bars shown side by side (cap)
    // Influence: copy one source step's vel/pitch/prob/roll onto every step in the channel.
    std::function<void(int ch, int srcStep)> onInfluenceApply;
    std::function<void(int ch)>              onInfluenceDisarm; // un-highlight the strip button
    MidiLearnManager* midiLearn = nullptr;

    int rowH = 44;            // set by parent layout
    double barMs = 2000.0;    // one bar in ms at the current tempo (set by the editor; Nudge ms read-out)
    int selectedRow = -1;     // the editor's selected channel - its row gets a soft highlight wash
    juce::Colour selectedRowColour { 0xffe8bf4d };   // that channel's colour (wash + left accent bar)
    int visibleRows = 8;      // how many channel rows to draw at once (the viewport height in rows)
    int firstRow = 0;         // scroll offset: the topmost drawn channel (maps ch -> screen row ch-firstRow)
    int currentPattern = 0;   // which pattern's steps/assignments we display

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;   // value modes: reset the step to its default
    void mouseMove(const juce::MouseEvent& e) override;          // DRAW mode: hover read-out of the note pitch
    juce::String getTooltip() override;   // explains edit modes + MERGE (cmd/shift+click) in one place

    void update(const Sequencer& seq, bool anySolo);
    // Close the piano-roll editor unless it belongs to `ch` (selecting another channel must not
    // leave a stale editor covering the grid - user report).
    void closeDrawEditorIfNot(int ch)
    { if (drawMagCh >= 0 && drawMagCh != ch) { drawMagCh = -1; prMode = 0; drawReadSemi = -128; prHoverSemi = -999; repaint(); } }
    float getRollDec(int ch, int step) const { return rollDec[ch][step]; }
    float getVel(int ch, int step) const { return vel[ch][step]; }   // velocity set by X-drag in Roll mode
    float getNoteLen(int ch, int step) const { return noteLen[ch][step]; }  // per-step gate length (Len mode)

private:
    static constexpr int NCH = Sequencer::NUM_CHANNELS;
    bool  steps[NCH][DrumChannel::MAX_STEPS] = {};
    float vel[NCH][DrumChannel::MAX_STEPS]   = {};
    float pit[NCH][DrumChannel::MAX_STEPS]   = {};
    int   roll[NCH][DrumChannel::MAX_STEPS]  = {};
    float rollDec[NCH][DrumChannel::MAX_STEPS] = {};   // roll decay 0..1 (fade across ratchet hits)
    float noteLen[NCH][DrumChannel::MAX_STEPS] = {};   // per-step GATE length 0..1 of one step (0 = off/natural)
    float pan[NCH][DrumChannel::MAX_STEPS]   = {};      // per-step stereo pan -1..+1 (Pan mode; 0 = centre)
    float nudge[NCH][DrumChannel::MAX_STEPS] = {};      // per-step micro-timing -1..+1 (Nudge mode; 0 = on the grid)
    float modA[NCH][DrumChannel::MAX_STEPS]  = {};      // STEP MOD LANE A: drawn 0..1 value per step (a mod source; 0 = none)
    float modB[NCH][DrumChannel::MAX_STEPS]  = {};      // STEP MOD LANE B
    bool  slide[NCH][DrumChannel::MAX_STEPS] = {};      // 303 slide flag (toggled in Pitch mode's bottom strip)
    bool  merge[NCH][DrumChannel::MAX_STEPS] = {};      // MERGE continuation flags (cmd/shift+click a step)
    int   condLen[NCH][DrumChannel::MAX_STEPS]  = {};   // per-step loop-condition cycle length (Prob mode)
    int   condMask[NCH][DrumChannel::MAX_STEPS] = {};   // per-step loop-condition bitmask (0 = every loop)
    int   condDragCh = -1, condDragStep = -1, condDownBar = -1, condDownX = 0;   // Prob-mode gesture state
    // [2026-07-15 23:00] per-NOTE loop-condition editor: a floating step-Loop-style cell opened
    // from the note menu. Drag = cycle length (1..5), click a segment = toggle that pass.
    int  condEdCh = -1, condEdIdx = -1;      // open when >= 0 (channel + note index)
    bool condEdSel = false;                  // apply edits to the whole selection
    juce::Rectangle<int> condEdRect;
    int  condEdDownBar = -1, condEdDownX = 0; bool condEdDragged = false, condEdDragging = false;
    int  rollMenuNoteCh = -1, rollMenuNoteIdx = -1;   // note whose right-click MENU is open (amber outline) [2026-07-15 23:40]
    bool  condDragged = false;
    int   curLoop = 0;          // the playing pattern's loop counter (highlights the current bar in Prob mode)
    bool  midiOutCh[NCH] = {};                         // is this channel routed to MIDI Out? (enables 2D Vel/Len)
    // MERGED-GROUP view: when the viewed pattern is in a merged group, every row shows the group's
    // bars SIDE BY SIDE - steps are the concatenation of each bar's steps (equal cell widths), the
    // piano roll spans grpBars * DRAW_RES columns. Edits go back to the right bar via the editor's
    // decode (concat step -> bar + local step; concat roll column -> bar + local column).
    int    grpBars = 1;
    int    barStep0[NCH][GRP_MAX + 1] = {};            // per channel: concat step where each bar begins (+ total)
    int    totalCols() const { return DrumChannel::DRAW_RES * grpBars; }

    // PIANO ROLL mirror + gesture state (a poly NOTE LIST replaces the step cells for this row).
    // MIR_MAX = capacity of the CONCATENATED group view (GRP_MAX bars of DRAW_MAX_NOTES each).
    static constexpr int MIR_MAX = DrumChannel::DRAW_MAX_NOTES * GRP_MAX;
    bool   drawMode[NCH] = {};
    DrumChannel::DrawNote drawNotes[NCH][MIR_MAX] = {};
    int    drawNoteCount[NCH] = {};
    int    drawDragCh = -1, drawLastCol = -1;          // channel being line-drawn + last column (interp)
    int    strokeNoteIdx = -1;                          // the note the current ROW line-stroke is extending
    bool   strokeCreatedNew = false;                    // this stroke APPENDED a note (vs extending an old one)
    int    tapNoteCh = -1, tapNoteIdx = -1;             // the note a single CLICK just created (a double-click
                                                        // removes it again before opening the magnifier)
    int    strokeLockSemi = -128;                       // the stroke's pitch = where the press LANDED (locked, like the magnified editor)
    bool   drawErase = false;                          // right-drag erases (removes notes under the stroke)
    float  playBarFrac = 0.0f;                         // current bar position 0..1 (piano-roll playhead)
    int    drawMagCh = -1;                             // channel whose BIG piano-roll OVERLAY is open (-1 = none)
    int    drawRange = 48;                             // overlay visible +/- range (48 / 24 / 12 / 6 semitones)
    int    drawReadSemi = -128;                        // live read-out semitone (-128 = none) shown top-right
    int    drawGridDiv = 16;                            // overlay SNAP grid: divisions of the bar (0 = free)
    int    prTargetSlot = 0;                            // DRAW TARGET: 0 = both slots (orange), 1 = slot 1 (yellow), 2 = slot 2 (pink)
    // Overlay pointer gestures: 0 = none, 1 = MOVE a note, 2 = RESIZE its right edge, 3 = CREATE new,
    // 4 = SCROLL the pitch view (drag the left note-name column when the range is < +-36),
    // 5 = MARQUEE area-select, 6 = MOVE the whole selection, 8 = PENDING right-click (menu if no drag, else marquee).
    int    prMode = 0, prIdx = -1, prGrabDCol = 0, prGrabDSemi = 0;
    int    prRightIdx = -1;                             // note under a pending right-click (prMode 8) - menu target on no-drag
    int    drawViewCenter = 0;                          // overlay pitch-view centre (semitones; 0 = C3 row centred)
    int    prScrollGrabY = 0, prScrollGrabC = 0;        // scroll-gesture anchors
    int    prHoverSemi = -999;                          // key row under the CURSOR (highlighted even over empty space)
    // MULTI-SELECT (piano-roll editor): RIGHT-DRAG = marquee (prMode 5); dragging a selected note
    // moves the whole selection (prMode 6). A right-click that DOESN'T drag opens the note menu.
    void   showRollNoteMenu(int ch2, int idx);          // the note right-click menu (called from mouseUp on a no-drag right-click)
    bool   prSel[MIR_MAX] = {};
    int    prSelCount = 0;
    juce::Point<int> prMarqA, prMarqB;                  // marquee corners while prMode == 5
    int32_t prOrigStart[MIR_MAX] = {};                  // originals for the group move (concat columns)
    int8_t  prOrigSemi[MIR_MAX]  = {};
    void   prClearSel() { if (prSelCount > 0) { for (auto& b : prSel) b = false; prSelCount = 0; } }
    int    prViewClamp() const { return DrumChannel::PITCH_RANGE - drawRange; }   // |center| max: window stays inside +-48
    float  dVel[NCH] = {};                             // piano-roll whole-channel default velocity mirror (drawVel)
    void   paintDrawLane(juce::Graphics& g, int ch, juce::Rectangle<int> rect, bool overlay);
    juce::Rectangle<int> drawRowRect(int ch) const;                  // the normal (1x) row rect
    juce::Rectangle<int> drawOverlayRect() const;                    // the BIG piano-roll editor panel
    static constexpr int PR_HEAD = 32, PR_KEYS = 30;                 // overlay header height (2x - small was unreadable) + note-name column width
    juce::Rectangle<int> prLane(juce::Rectangle<int> ov) const       // the note area inside the overlay
    { return ov.withTrimmedTop(PR_HEAD).withTrimmedLeft(PR_KEYS); }
    int    yToDrawSemi(juce::Rectangle<int> rect, int y, int range, int centre = 0) const;   // pixel Y -> semitone (view-centre aware)
    void   drawStrokeTo(int ch, juce::Point<int> pos);               // ROW line gesture (erase-under + extend note)
    int    drawColAt(int x) const;                                   // pixel X -> column (row rect)
    int    prColAt(juce::Rectangle<int> lane, int x) const           // pixel X -> concat column (overlay lane)
    { return juce::jlimit(0, totalCols() - 1,
             (int) ((float)(x - lane.getX()) / (float) juce::jmax(1, lane.getWidth()) * (float) totalCols())); }
    int    prSnap(int col) const                                     // snap a concat column to the overlay grid
    { if (drawGridDiv <= 0) return col; const int cw = DrumChannel::DRAW_RES / drawGridDiv;   // grid repeats per BAR
      return juce::jlimit(0, totalCols() - 1, (col / cw) * cw); }
    int    prNoteAt(int ch, int col, int semi) const;                // topmost note covering (col, semi) or -1
    void   eraseColRange(int ch, int lo, int hi);                    // remove/trim/split notes crossing [lo..hi]
    void   pushNotes(int ch)                                         // mirror -> channel (whole list)
    { if (onDrawNotesChanged) onDrawNotesChanged(ch, drawNotes[ch], drawNoteCount[ch]); }
    bool   muted[NCH]       = {};
    bool  soloed[NCH]      = {};
    int   numSteps[NCH]    = {};   // filled by update(); 0 until then
    int   playStep[NCH]    = {};   // filled by update(); 0 here is harmless (no cursor drawn until playing)
    bool  anySolo        = false;
    int   dragChannel    = -1;   // channel of the in-progress value drag (for Influence)
    int   dragStep       = -1;   // step the value drag started on - LOCKED for the drag (no mid-drag re-pick)
    void  handleValueDrag(juce::Point<int> pos);
    void  applyInfluence(int ch, int srcStep); // copy srcStep onto every step in the channel

public:
    // STEP MAGNIFIER (value modes): while the mouse is down on a step, that cell is drawn (and
    // its value mapped) enlarged - 2x when the channel has <= 15 steps (cells are already big),
    // 3.5x from 16 steps up - ANCHORED so the cursor keeps its fractional position inside the
    // cell, then CLAMPED into the editor bounds (edge cells used to poke outside the plugin
    // window; clamping shifts the anchor there, visibility wins). Cleared on mouseUp.
    int   magCh = -1, magStep = -1;
    juce::Rectangle<int> magRect;
    juce::Component* magOverlay = nullptr;   // top-most sibling that paints the magnified cell above the top bar
    void notifyMag() { repaint(); if (magOverlay) magOverlay->repaint(); }
    void beginMagnify(int ch, int step, juce::Point<int> pos)
    {
        const auto r = stepRect(ch, step);
        const float fx = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
        const float fy = (float)(pos.y - r.getY()) / (float) juce::jmax(1, r.getHeight());
        const bool few = numSteps[ch] <= 15;    // big cells need less magnification
        // [2026-07-15 23:00] the VERTICAL scale is one factor bigger than the horizontal (user):
        // <= 15 steps = 2x wide / 3x tall; 16+ = 3.5x wide / 4.5x tall.
        const int mw = few ? r.getWidth()  * 2 : (r.getWidth()  * 7) / 2;
        const int mh = few ? r.getHeight() * 3 : (r.getHeight() * 9) / 2;
        magCh = ch; magStep = step;
        magRect = { pos.x - juce::roundToInt(fx * (float) mw), pos.y - juce::roundToInt(fy * (float) mh), mw, mh };
        if (auto* p = getParentComponent())   // keep it fully inside the editor (content) bounds
        {
            const auto lim = p->getLocalBounds() - getPosition();   // content rect in grid coords
            magRect.setX(juce::jlimit(lim.getX(), juce::jmax(lim.getX(), lim.getRight()  - mw), magRect.getX()));
            magRect.setY(juce::jlimit(lim.getY(), juce::jmax(lim.getY(), lim.getBottom() - mh), magRect.getY()));
        }
        notifyMag();
    }
    void endMagnify() { if (magCh >= 0) { magCh = magStep = -1; notifyMag(); } }
    // The rect a gesture maps positions against: the magnified rect while this step is magnified.
    juce::Rectangle<int> activeStepRect(int ch, int step) const
    { return (ch == magCh && step == magStep) ? magRect : stepRect(ch, step); }
    // The magnified cell (in THIS grid's local coords); false if none. Used by the overlay to
    // re-draw it above everything (the top bar / channel strips clip a first-row magnified cell).
    bool magnifiedCell(int& ch, int& step, juce::Rectangle<int>& rr) const
    {
        const int lastR = juce::jmin(firstRow + visibleRows, Sequencer::NUM_CHANNELS);
        if (editMode == ModeSteps || magCh < juce::jmax(0, firstRow) || magCh >= lastR
            || magStep < 0 || magStep >= numSteps[magCh]) return false;
        ch = magCh; step = magStep; rr = magRect; return true;
    }
    void paintValueCell(juce::Graphics& g, int ch, int step, juce::Rectangle<int> rr, bool magnified);

private:
    juce::String stepParamId(int ch, int step) const;
    juce::Rectangle<int> stepRect(int ch, int step) const;
    bool findStepAt(juce::Point<int> pos, int& outCh, int& outStep) const;
    void handleClick(juce::Point<int> pos, bool setDragState);
    void paintCellExtras(juce::Graphics& g, int ch, int step, juce::Rectangle<int> rr,
                         juce::Rectangle<float> r, bool isActive, bool isCurrent);
    void paintMergeArrow(juce::Graphics& g, int ch, int step, juce::Rectangle<float> r);
    bool lastDragState = false;
};

//==============================================================================
// Top-most transparent overlay that redraws the step grid's MAGNIFIED cell in front of
// everything (the top bar and channel strips otherwise clip a first-row / first-column
// magnified cell). Mouse-transparent; shares the grid's parent coordinate space.
class StepMagnifierOverlay : public juce::Component
{
public:
    StepGridComponent* grid = nullptr;
    StepMagnifierOverlay() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override
    {
        if (grid == nullptr) return;
        int ch, step; juce::Rectangle<int> rr;
        if (! grid->magnifiedCell(ch, step, rr)) return;
        // grid + overlay are siblings -> grid-local + grid.getPosition() == overlay-local.
        grid->paintValueCell(g, ch, step, rr.translated(grid->getX() - getX(), grid->getY() - getY()), true);
    }
};

//==============================================================================
// Full-canvas recording count-in: a big, HALF-TRANSPARENT "3 / 2 / 1 / GO!" drawn over the whole
// editor while a 3 s count-in runs. Mouse-transparent + kept top-most (toFront in layout).
class CountdownOverlay : public juce::Component
{
public:
    juce::String label;   // "3" / "2" / "1" / "GO!"  (empty = nothing drawn)
    CountdownOverlay() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override
    {
        if (label.isEmpty()) return;
        auto b = getLocalBounds();
        g.fillAll(juce::Colour(0x22000000));   // faint scrim so the digit reads over any UI, still see-through
        const bool go = (label == "GO!");
        const float h = (float) juce::jmin(b.getWidth(), b.getHeight()) * (go ? 0.34f : 0.55f);
        g.setFont(juce::Font(h, juce::Font::bold));
        g.setColour(go ? juce::Colour(0x8853e08a)      // green ~53% alpha
                       : juce::Colour(0x88ffffff));     // white ~53% alpha = "half transparent"
        g.drawText(label, b, juce::Justification::centred, false);
    }
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
// Shows the current assignment ("Assigned: ch6 cc44" / "Not assigned"). altPid/altLabel add an
// optional SECOND target to the same menu (the "SELECTED channel" variants on M/S/OV);
// alt2Pid/alt2Label a THIRD (steps' "this cell in the SELECTED pattern" variant [2026-07-14 11:10]).
void showMidiLearnMenu(juce::Component* target, MidiLearnManager& mlm,
                       const juce::String& paramId, int forcedChannel,
                       const juce::String& altPid = {}, const juce::String& altLabel = {},
                       const juce::String& alt2Pid = {}, const juce::String& alt2Label = {});

//==============================================================================
// Knob with right-click MIDI-learn popup.
class LearnableKnob : public juce::Slider
{
public:
    LearnableKnob(const juce::String& paramId, MidiLearnManager& mlm);
    void mouseDown(const juce::MouseEvent& e) override;
    // MODULATION RING (Vital-style): -1 = none; else the live modulated position (0..1 of the range).
    // The knob's thumb stays at the user's base value; a cyan arc shows where modulation pushes it.
    float modRing = -1.0f;
    void setModRing(float v) { if (std::abs(v - modRing) > 0.003f) { modRing = v; repaint(); } }
    void paint(juce::Graphics& g) override
    {
        juce::Slider::paint(g);
        if (modRing >= 0.0f && (getSliderStyle() == juce::Slider::LinearHorizontal
                             || getSliderStyle() == juce::Slider::LinearVertical
                             || getSliderStyle() == juce::Slider::LinearBar))
        {   // LINEAR faders (the Osc FM/Warp/Freq controls + stacked engine faders): a cyan band from
            // the base value to the live modulated value + a bright marker at the modulated position.
            const float b0 = (float) getPositionOfValue(getValue());
            const float b1 = (float) getPositionOfValue(proportionOfLengthToValue(juce::jlimit(0.0, 1.0, (double) modRing)));
            g.setColour(juce::Colour(0xff35c0ff));
            if (isHorizontal())
            {
                const float y = (float) getHeight() * 0.5f;
                g.setColour(juce::Colour(0x5535c0ff)); g.fillRect(juce::Rectangle<float>(juce::jmin(b0, b1), y - 4.0f, std::abs(b1 - b0), 8.0f));
                g.setColour(juce::Colour(0xff35c0ff)); g.fillRect(b1 - 1.2f, y - 6.0f, 2.4f, 12.0f);
            }
            else
            {
                const float x = (float) getWidth() * 0.5f;
                g.setColour(juce::Colour(0x5535c0ff)); g.fillRect(juce::Rectangle<float>(x - 4.0f, juce::jmin(b0, b1), 8.0f, std::abs(b1 - b0)));
                g.setColour(juce::Colour(0xff35c0ff)); g.fillRect(x - 6.0f, b1 - 1.2f, 12.0f, 2.4f);
            }
        }
        if (modRing >= 0.0f && (getSliderStyle() == juce::Slider::RotaryVerticalDrag
                             || getSliderStyle() == juce::Slider::Rotary
                             || getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag))
        {   // arc from the base value (thumb) to the live modulated value
            const auto rp = getRotaryParameters();
            const float span = rp.endAngleRadians - rp.startAngleRadians;
            const float base01 = (float) juce::jlimit(0.0, 1.0, valueToProportionOfLength(getValue()));
            const float a0 = rp.startAngleRadians + base01 * span;
            const float a1 = rp.startAngleRadians + juce::jlimit(0.0f, 1.0f, modRing) * span;
            if (std::abs(a1 - a0) > 0.01f) {
                auto r = getLocalBounds().toFloat();
                const float sz = juce::jmin(r.getWidth(), r.getHeight());
                const float cx = r.getCentreX(), cy = r.getY() + sz * 0.5f, rad = sz * 0.5f - 1.5f;
                juce::Path arc; arc.addCentredArc(cx, cy, rad, rad, 0.0f, a0, a1, true);
                g.setColour(juce::Colour(0xff35c0ff));
                g.strokePath(arc, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
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
    // Optional SECOND learn target (the "SELECTED channel - follows selection" variant on the
    // strip M/S/OV buttons); the right-click menu offers both when set.
    juce::String altPid, altLabel;
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
    bool snapRatio = false;      // FM Ratio: snap the mapped 1..6x ratio to integers (hold Shift = free)
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
    std::function<void()> onOpenCustom;            // Wave = Custom: click the drawing = open the harmonic editor
    std::function<const float*()> getCustomTbl;        // -> the STATIC preview table (the position blend)
    std::function<const float*(int)> getCustomFrame;   // -> one baked frame table (0..3) for the in-motion split view
    void mouseEnter(const juce::MouseEvent&) override { hoverEd = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hoverEd = false; repaint(); }
    bool hoverEd = false;                          // hover = show the "click to edit" affordance (Custom only)
    bool fmMode = false;                           // FM: apply phase modulation to the drawing
    bool compact = false;                          // short form (no A/B label strip) when the box is tight
    // LIVE MODULATION overrides: while the matrix modulates FM Amount / Ratio / Warp, the drawn wave
    // follows the MODULATED values (fed per timer tick from the DSP snapshot; -1000 = not modulated).
    float modFm = -1000.0f, modRatio = -1000.0f, modWarp = -1000.0f;
    float modSync = -1000.0f, modBend = -1000.0f;   // [2026-07-18] the shape trio's live-modulated values
    void setModLive(float fm, float ratio, float warp, float syncV = -1000.0f, float bendV = -1000.0f)
    { if (std::abs(fm - modFm) > 0.004f || std::abs(ratio - modRatio) > 0.004f || std::abs(warp - modWarp) > 0.004f
          || std::abs(syncV - modSync) > 0.004f || std::abs(bendV - modBend) > 0.004f)
      { modFm = fm; modRatio = ratio; modWarp = warp; modSync = syncV; modBend = bendV; repaint(); } }
    // [2026-07-16 round-5] LIVE WAVETABLE POSITION on the slot preview (user ask): the engine's
    // real playing position (knob + glide + WAVE LFO, same feed as the draw window's amber
    // marker) as a thin amber marker along the bottom. -1 = none (idle / not a Custom wave).
    float wtPosLive = -1.0f;
    void setWtPosLive(float p)
    { if (std::abs(p - wtPosLive) > 0.004f && ! (p < 0.0f && wtPosLive < 0.0f)) { wtPosLive = p; repaint(); } }
    // GRANULAR: live grain read positions (0..1 across the source) + the source caption. The
    // dots are REAL grain positions from the engine (honest-visual rule); -1 = not a grain slot.
    float grainPos[DrumChannel::GRAINS_MAX] = {}; int grainN = -1;
    juce::String srcName;                          // "Sample: x.wav" while granulating a file
    void setGrainLive(const float* p, int n, const juce::String& nm)
    {
        bool ch = (n != grainN) || (nm != srcName);
        for (int i = 0; i < n && ! ch; ++i) ch = std::abs(p[i] - grainPos[i]) > 0.01f;
        if (! ch) return;
        grainN = n; srcName = nm;
        for (int i = 0; i < n; ++i) grainPos[i] = p[i];
        repaint();
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    static float basicWave(int shape, float ph01);
};

//==============================================================================
// ADDITIVE WAVETABLE (Wave = "Custom"): the DRAW HARMONICS overlay, now FOUR frames (A/B/C/D)
// in a 2x2 grid with a POSITION strip between the rows. Each frame = 32 harmonic bars + a freehand
// wave strip + a shape-recipe dropdown + "Analyze sample" (pitch-detect a file -> its harmonics).
// Position scans A -> D (the two neighbouring frames crossfade); the Glide box = per-note travel
// 0 -> 1 over N seconds; the WAVE LFO (LFO visual, 4th tab) scans position live. An in-editor
// overlay (a `content` child, never an OS popup - the popup rule), closed by its X or layoutContent.
class HarmonicEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onChange;               // read vals/phs/morphSec/wtPos back into the slot
    std::function<void()> onDragEnd;              // released (audition + undo hash)
    std::function<void()> onClose;
    juce::Component* clickIgnore = nullptr;       // the wave preview that opened us (its click re-opens)
    juce::Colour accent { 0xffe8bf4d };           // slot colour (yellow 1 / pink 2)
    int slotIdx = 0;                              // which slot is being drawn (title)
    ~HarmonicEditor() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }
    void visibilityChanged() override             // click-outside closes, like every other dropdown/popup
    {
        if (isVisible() && ! closerHooked)      { juce::Desktop::getInstance().addGlobalMouseListener(&closer); closerHooked = true; }
        else if (! isVisible() && closerHooked) { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); closerHooked = false; }
    }
    static constexpr int NF = DrumChannel::ADD_FRAMES;   // 4 frames
    float vals[NF][DrumChannel::ADD_HARM] = { { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f } };
    float phs [NF][DrumChannel::ADD_HARM] = {};
    float seg[NF - 1] = {};                       // per-leg glide seconds (0 = HOLD); [0] == 0 = glide off
    bool  loopOn = false;                         // ping-pong the glide forever
    float wtPos = 0.0f;                           // static wavetable position 0..1
    // LIVE position marker (amber line on the Position strip): the REAL played position from the
    // engine (handle + glide + WAVE LFO), fed by the editor timer; -1 = nothing playing.
    void setLivePos(float p)
    { if (std::abs(p - livePos) > 0.004f || (p < 0.0f) != (livePos < 0.0f))
      { livePos = p; repaint(posRect().getSmallestIntegerContainer().expanded(3)); } }
    void setValues(const float (*h)[DrumChannel::ADD_HARM], const float (*p)[DrumChannel::ADD_HARM],
                   const float* segs, float pos, bool loopIn)
    { for (int f = 0; f < NF; ++f) for (int i = 0; i < DrumChannel::ADD_HARM; ++i)
      { vals[f][i] = h[f][i]; phs[f][i] = p[f][i]; }
      for (int k = 0; k < NF - 1; ++k) seg[k] = segs[k];
      wtPos = pos; loopOn = loopIn;
      for (int f = 0; f < NF; ++f) { frameLabel[f].clear(); rebuildStrokeFromHarmonics(f); detectLoadedShape(f); }
      repaint(); }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override
    { if ((drawing || waveDrawing || segDragging >= 0 || posDragging) && onDragEnd) onDragEnd();
      drawing = false; waveDrawing = false; segDragging = -1; posDragging = false; waveLastI = -1; }
    juce::String getTooltip() override;
private:
    static constexpr int WPTS = 256;              // freehand wave stroke resolution (one cycle)
    float wavePts[NF][WPTS] = {};
    float livePos = -1.0f;                        // live played position (-1 = none; see setLivePos)
    bool  drawing = false, waveDrawing = false, posDragging = false;
    int   segDragging = -1;                       // which leg's time box is being dragged (-1 = none)
    int   dragFrame = 0, waveLastI = -1;
    struct Closer : juce::MouseListener
    {
        HarmonicEditor& ed; explicit Closer(HarmonicEditor& e) : ed(e) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! ed.isVisible() || ed.menuOpen) return;   // a shape menu / file chooser must not close the editor
            const auto p = e.getScreenPosition();
            if (ed.getScreenBounds().contains(p)) return;
            if (ed.clickIgnore != nullptr && ed.clickIgnore->getScreenBounds().contains(p)) return;
            ed.setVisible(false);
        }
    };
    Closer closer { *this };
    bool closerHooked = false;
    bool menuOpen = false;                        // a shape PopupMenu / file chooser is up - the closer waits
    std::unique_ptr<juce::FileChooser> chooser;   // "Analyze sample" file picker (async, member = stays alive)
    juce::String frameLabel[NF];                  // analyzed-sample name shown in the dropdown ("" = shape/Custom)
    juce::String frameMsg[NF];                    // transient per-frame notice ("no clear pitch found")
    int  loadedShape[NF] = { -1, -1, -1, -1 };    // factory shape index this frame holds (-1 = Custom)
    float* HV(int f) { return vals[f]; }
    float* HP(int f) { return phs[f]; }
    float* WP(int f) { return wavePts[f]; }
    juce::Colour frameCol(int f) const            // A = slot accent, B cyan, C violet, D green
    { const juce::Colour c[NF - 1] = { juce::Colour(0xff35c0ff), juce::Colour(0xffb46bff), juce::Colour(0xff35b56a) };
      return f == 0 ? accent : c[f - 1]; }
    // ---- 2x2 geometry: header row / frame row / POSITION band / frame row / footer hint ----
    static constexpr float HDR = 20.0f, BAND = 30.0f, FOOT = 14.0f;
    float colW() const { return ((float) getWidth() - 24.0f) / 2.0f; }
    float colX(int c2) const { return 8.0f + (float) c2 * (colW() + 8.0f); }
    float rowH() const { return ((float) getHeight() - HDR - BAND - FOOT) * 0.5f; }
    float rowY(int r) const { return HDR + (float) r * (rowH() + BAND); }
    juce::Rectangle<float> frameRect(int f) const { return { colX(f & 1), rowY(f >> 1), colW(), rowH() }; }
    int frameAt(juce::Point<float> p) const
    { for (int f = 0; f < NF; ++f) if (frameRect(f).contains(p)) return f; return -1; }
    juce::Rectangle<float> closeRect() const { return { (float) getWidth() - 24.0f, 2.0f, 20.0f, 16.0f }; }
    juce::Rectangle<float> shapeBtnRect(int f) const { auto r = frameRect(f); return { r.getX() + 16.0f, r.getY(), 128.0f, 15.0f }; }
    juce::Rectangle<float> smpRect(int f) const      { auto r = frameRect(f); return { r.getX() + 148.0f, r.getY(), 96.0f, 15.0f }; }
    juce::Rectangle<float> clrRect(int f) const      { auto r = frameRect(f); return { r.getX() + 248.0f, r.getY(), 40.0f, 15.0f }; }
    juce::Rectangle<float> waveStripRect(int f) const { auto r = frameRect(f); return { r.getX(), r.getY() + 18.0f, r.getWidth(), 46.0f }; }
    juce::Rectangle<float> barArea(int f) const
    { auto r = frameRect(f); return { r.getX(), r.getY() + 66.0f, r.getWidth(), r.getHeight() - 66.0f - 12.0f }; }
    // POSITION band (between the rows): the scan strip + THREE per-leg glide time boxes
    // (A>B / B>C / C>D; hard left = HOLD = the note stops at that leg's left frame)
    juce::Rectangle<float> posRect() const
    { return { 78.0f, rowY(0) + rowH() + 6.0f, (float) getWidth() - 78.0f - 364.0f, 18.0f }; }
    juce::Rectangle<float> segRect(int k) const
    { return { (float) getWidth() - 356.0f + (float) k * 100.0f, rowY(0) + rowH() + 6.0f, 94.0f, 18.0f }; }
    juce::Rectangle<float> loopRect() const   // LOOP toggle, right of the C>D box (user placement)
    { return { (float) getWidth() - 52.0f, rowY(0) + rowH() + 6.0f, 44.0f, 18.0f }; }
    void applyAt(const juce::MouseEvent& e, bool erase, int f);
    void strokeToHarmonics(int f);                // DFT the drawn cycle -> vals + phs (32 partials)
    void rebuildStrokeFromHarmonics(int f);       // vals + phs -> wavePts (band-limited reconstruction)
    void loadShapeSpectrum(int f, int shape);     // place a factory recipe into this frame
    void detectLoadedShape(int f);                // name the frame if its bars match a factory shape
    void analyzeSample(int f);                    // file chooser -> pitch detect -> harmonics into frame f
};

//==============================================================================
// LFO SHAPER overlay: draw ONE cycle of the Custom LFO shape on a big strip (the in-view drag
// clashed with the wave's rate/amount gesture - user). Opened by picking "Custom" in the Shape
// list or clicking the wave while Custom. Content child + click-outside Closer (the popup rule).
class LfoCurveEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int CVN = 64;
    float curve[CVN] = {};
    int  grid = 16;       // [2026-07-16] draw grid, 1..32 cells (saved with the sound per LFO)
    bool snap = false;    // SNAP toggle: on = flatten + draw as flat per-cell steps
    juce::Colour accent { 0xffff9a3c };
    std::function<void(const float*)> onChange;
    std::function<void(int grid, bool snap)> onToolsChange;   // editor persists Grid/Snap onto the slot
    std::function<void()> onClose;
    juce::Component* clickIgnore = nullptr;
    ~LfoCurveEditor() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }
    void visibilityChanged() override
    {
        if (isVisible() && ! closerHooked)      { juce::Desktop::getInstance().addGlobalMouseListener(&closer); closerHooked = true; }
        else if (! isVisible() && closerHooked) { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); closerHooked = false; }
    }
    void setCurve(const float* cv) { for (int k = 0; k < CVN; ++k) curve[k] = cv[k]; repaint(); }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override { lastI = -1; gridDrag = false; }
    void flatten();   // quantize the current curve into `grid` flat cells (snap-on behaviour)
    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();
        if (gridRect().contains(p))
            return "GRID: how many cells one LFO cycle is divided into (drag, 1-32; odd counts = "
                   "polyrhythms). With SNAP on, drawing on this grid does the same job as step "
                   "modulation (Mod A/B) - but it is part of the SOUND, so it works in the Piano "
                   "Roll too and survives switching modes. Pair with Sync = Bar for one cycle per bar.";
        if (snapRect().contains(p))
            return "SNAP: while ON, the drawing flattens into one level per Grid cell (piano-roll "
                   "style steps) and new strokes land as flat cells. Off = freehand drawing.";
        return "Draw ONE cycle of this LFO's movement (left-drag). It plays at the LFO's speed and is "
               "scaled by its Amount; Sync / Retrig / Free all apply. Click outside to close.";
    }
private:
    struct Closer : juce::MouseListener
    {
        LfoCurveEditor& ed; explicit Closer(LfoCurveEditor& e) : ed(e) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! ed.isVisible()) return;
            const auto p = e.getScreenPosition();
            if (ed.getScreenBounds().contains(p)) return;
            if (ed.clickIgnore != nullptr && ed.clickIgnore->getScreenBounds().contains(p)) return;
            ed.setVisible(false); if (ed.onClose) ed.onClose();
        }
    };
    Closer closer { *this };
    bool closerHooked = false;
    int  lastI = -1;
    bool gridDrag = false;
    juce::Rectangle<float> closeRect() const { return { (float) getWidth() - 24.0f, 4.0f, 20.0f, 16.0f }; }
    juce::Rectangle<float> gridRect() const { return { (float) getWidth() - 250.0f, 3.0f, 130.0f, 18.0f }; }   // "Grid 16" drag box
    juce::Rectangle<float> snapRect() const { return { (float) getWidth() - 112.0f, 3.0f, 70.0f, 18.0f }; }    // "Snap" toggle
    juce::Rectangle<float> strip() const { return getLocalBounds().toFloat().reduced(10.0f).withTrimmedTop(26.0f).withTrimmedBottom(16.0f); }
};

//==============================================================================
// RemapEditor [2026-07-14 00:03]: the per-route REMAP curve overlay (Vital-style "modulation
// remap"). X = the source's incoming value, Y = what actually reaches the target; the drawn shape
// replaces the pass-through diagonal FOR THIS ROUTE ONLY. One mechanism for every route.
//==============================================================================
class RemapEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int N = 64;
    float curve[N] = {}; bool on = false;
    bool bipolarSrc = false;                     // bipolar source: X spans -1..+1 (centre = at rest)
    juce::Colour accent { 0xffe8bf4d };
    juce::String title;
    std::function<void(const float*, bool)> onChange;
    std::function<void()> onClose;
    juce::Component* clickIgnore = nullptr;
    ~RemapEditor() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }
    void openFor(const juce::String& t, const float* cv, bool isOn, bool bipolar, juce::Colour ac)
    {
        title = t; on = isOn; bipolarSrc = bipolar; accent = ac;
        if (isOn) for (int k = 0; k < N; ++k) curve[k] = cv[k];
        else      for (int k = 0; k < N; ++k) curve[k] = (float) k / (float)(N - 1);   // identity
        setVisible(true); toFront(false); repaint();
    }
    void visibilityChanged() override
    {
        if (isVisible() && ! closerHooked)      { juce::Desktop::getInstance().addGlobalMouseListener(&closer); closerHooked = true; }
        else if (! isVisible() && closerHooked) { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); closerHooked = false; }
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override { lastI = -1; }
    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();
        if (presetRect(0).contains(p)) return "LINEAR: turn the remap OFF - the source passes through unchanged.";
        if (presetRect(1).contains(p)) return "SOFT: the effect arrives LATE - light source values do almost nothing (tame a hair-trigger aftertouch, make velocity gentler).";
        if (presetRect(2).contains(p)) return "HARD: the effect arrives EARLY - even light source values push the target far.";
        if (presetRect(3).contains(p)) return "S-CURVE: a smooth switch - quiet low zone, fast transition through the middle, settled top.";
        return juce::String("MODULATION AMOUNT MAP: draw this route's transfer curve (left-drag).\n\n")
             + (bipolarSrc ? "- X = the source's full -1..+1 sweep (CENTRE = the source at rest).\n"
                           : "- X = the source's value from 0 (left) to full (right).\n")
             + "- Y = what actually reaches the target (bottom = nothing, top = full).\n"
             + "- The thin diagonal = pass-through; drawing anything switches the remap ON.\n"
             + "- Click outside to close.";
    }
private:
    struct Closer : juce::MouseListener
    {
        RemapEditor& ed; explicit Closer(RemapEditor& e) : ed(e) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! ed.isVisible()) return;
            const auto p = e.getScreenPosition();
            if (ed.getScreenBounds().contains(p)) return;
            if (ed.clickIgnore != nullptr && ed.clickIgnore->getScreenBounds().contains(p)) return;
            ed.setVisible(false); if (ed.onClose) ed.onClose();
        }
    };
    Closer closer { *this };
    bool closerHooked = false;
    int  lastI = -1;
    juce::Rectangle<float> closeRect()  const { return { (float) getWidth() - 24.0f, 4.0f, 20.0f, 16.0f }; }
    juce::Rectangle<float> presetRect(int i) const   // [2026-07-14 01:20] 4x2 preset grid
    { return { 10.0f + (float)(i % 4) * 62.0f, 22.0f + (float)(i / 4) * 19.0f, 58.0f, 16.0f }; }
    juce::Rectangle<float> strip() const { return getLocalBounds().toFloat().reduced(10.0f).withTrimmedTop(62.0f).withTrimmedBottom(12.0f); }
    void applyPreset(int i);
    void drawAt(juce::Point<float> pos, bool connect);
    friend class DrumSequencerEditor;
};

//==============================================================================
// RoutePicker: the two-column SOURCE | TARGET chooser for ONE mod route. Opened by RIGHT-CLICKING a
// fader in the MODULATION matrix. A content-child overlay (never an OS popup): left column = every
// source, right column = every target (incl. this engine's own knobs). Click a row in each column;
// it live-applies. Closes on an outside click. (The SoundPickerPanel two-column-list pattern.)
class RoutePicker : public juce::Component,
                    public juce::SettableTooltipClient   // [2026-07-13 22:50] per-ITEM tips (hover a row)
{
public:
    std::function<void(int src, int tgt)> onPicked;         // chosen source enum + target enum (live-apply)
    std::function<void(float amt)> onAmt;                   // route AMOUNT edited on the picker's own fader
    std::function<void(float ms)>  onLag;                   // [2026-07-14 01:33] route LAG edited (ms; 0 = instant)
    std::function<void()> onRemap;                          // [2026-07-14] open the MOD AMOUNT MAP editor for this route
    bool remapOn = false;                                   // drawn map active (shown on the map row)
    std::function<void()> onMidi;                           // [2026-07-14 11:10] MIDI-assign this route's AMOUNT (learn menu)
    juce::String midiTag { "MIDI" };                        // "MIDI" / "MIDI cc44" (set by the editor from the learn map)
    std::function<void()> onAmtDragEnd;                     // amount fader released (auto-audition)
    std::function<void()> onClose;
    std::function<juce::String(int gridIdx)> gridKnobName;  // live engine-knob names for grid targets
    juce::Colour accent { 0xffe8bf4d };
    int engine = -1;   // the slot's engine (set before openFor) - gates engine-specific targets (Warp = Osc only)
    bool rollMode = false;   // [2026-07-16] channel in PIANO ROLL: Step Mod A/B rows FADE with a reason
                             // (their lanes are step data - wiped by the mode switch, undrawable in the roll)
    bool rowDisabled(bool isSrc, int id) const
    { return isSrc && rollMode && (id == DrumChannel::MSStepModA || id == DrumChannel::MSStepModB); }
    RoutePicker();
    ~RoutePicker() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }
    void openFor(int curSrc, int curTgt, float amt, float lagMs = 0.0f);   // (re)fill both lists, select current, become visible
    void setLagFromX(float x);                              // [2026-07-14 01:33] the LAG fader drag
    void visibilityChanged() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;     // the bottom AMOUNT fader
    juce::String getTooltip() override;                     // per-row SOURCE/TARGET tips (rowAt hover, the TipList idiom)
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
private:
    bool amtDrag = false, lagDrag = false;
    struct Col : public juce::ListBoxModel
    {
        RoutePicker& owner; bool isSrc;
        Col(RoutePicker& o, bool s) : owner(o), isSrc(s) {}
        std::vector<std::pair<int, juce::String>> rows;     // (enum id, label)
        int getNumRows() override { return (int) rows.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        // [2026-07-13 23:20] THE per-row tooltip hook. JUCE list rows are their own components, so a
        // getTooltip() on the parent picker is never consulted - the MODEL must serve row tips.
        juce::String getTooltipForRow(int row) override;
    };
    Col srcModel { *this, true }, tgtModel { *this, false };
    juce::ListBox srcList, tgtList;
    juce::Label lblSrc, lblTgt;
    int curSrc = 0, curTgt = 0;
    float curAmt = 0.0f;                    // the route's amount (edited on the bottom fader)
    float curLag = 0.0f;                    // the route's LAG in ms (0 = instant)
    // [2026-07-14 01:33] bottom row = TWO half-width faders (user): LAG under the SOURCE column,
    // AMOUNT under the TARGET column.
    juce::Rectangle<float> lagRect() const
    { const float pad = 6.0f, h = 26.0f;
      return { pad, (float) getHeight() - h - pad, (float) getWidth() * 0.5f - pad * 1.5f, h }; }
    juce::Rectangle<float> amtRect() const
    { const float pad = 6.0f, h = 26.0f;
      return { (float) getWidth() * 0.5f + pad * 0.5f, (float) getHeight() - h - pad,
               (float) getWidth() * 0.5f - pad * 1.5f, h }; }
    juce::Rectangle<float> remapRect() const   // [2026-07-14] the MOD AMOUNT MAP row (left 2/3; MIDI button takes the right)
    { const float pad = 6.0f;
      return { pad, (float) getHeight() - 26.0f - pad - 24.0f, (float) getWidth() * 0.68f - pad, 20.0f }; }
    juce::Rectangle<float> midiRect() const    // [2026-07-14 11:10] MIDI-assign button, right of the map row
    { const float pad = 6.0f; const auto rr = remapRect();
      return { rr.getRight() + pad, rr.getY(), (float) getWidth() - rr.getRight() - pad * 2.0f, 20.0f }; }
    void setAmtFromX(float x);
    void selectCurrentRows();
    struct Closer : public juce::MouseListener
    {
        RoutePicker& p; explicit Closer(RoutePicker& r) : p(r) {}
        void mouseDown(const juce::MouseEvent& e) override
        { if (! p.isVisible()) return;
          const auto sp = e.getScreenPosition();
          if (p.getScreenBounds().contains(sp)) return;
          p.setVisible(false); if (p.onClose) p.onClose(); }
    };
    Closer closer { *this };
    bool hooked = false;
    friend struct Col;
};

//==============================================================================
// ModFaderMatrix: the MODULATION box's 12 route faders (6 rows x 2 columns). Each fader = one MOD
// route: RIGHT-CLICK opens the two-column source|target picker; DRAG / click inside sets the bipolar
// amount (centre = 0, left negative, right positive; double-click = 0). The fader shows
// "Source -> Target" (a drawn arrow between them) and fills from the centre by the amount. Yellow
// for sound-slot 1, pink for slot 2. Reads/writes the current slot's mod[12]. Everything is inline
// now (this replaces both the old ROUTE panel and the MATRIX overlay).
class ModFaderMatrix : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int NR = DrumChannel::MOD_ROUTES;      // 12
    juce::Colour accent { 0xffe8bf4d };
    int  slotIdx = 0;
    bool rollMode = false;   // [2026-07-16] roll channel: Step Mod routes draw DIMMED (their lanes are step-mode-only)
    int   src[NR] = {}, tgt[NR] = {};
    float amt[NR] = {};
    bool  cvOn[NR] = {};   // [2026-07-14] this route has a REMAP curve (small glyph on the fader)
    std::function<void()> onChange;                 // write src/tgt/amt back into the slot
    std::function<void()> onDragEnd;                // released (auto-audition)
    std::function<void(int route)> onPickRoute;     // right-click a fader -> open the picker for it
    std::function<juce::String(int gridIdx)> gridKnobName;
    void setValues(const DrumChannel::Slot& sl);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    juce::String getTooltip() override
    { return "12 modulation routes (this slot).\n\n"
             "- RIGHT-CLICK a fader: choose its SOURCE (left column) and TARGET (right column), set the "
             "amount + LAG on the two faders, and DRAW a per-route MODULATION AMOUNT MAP on the row "
             "above them (a tiny blue curve mark on a fader = mapped). The MIDI button beside the map "
             "row assigns a CC (knob/fader) to that route's amount.\n"
             "- DRAG / click inside a fader: the amount - centre is 0, left negative, right positive "
             "(double-click = 0).\n"
             "- Yellow = slot 1, pink = slot 2. Per-voice on chords.\n"
             "- AUDIO RATE: pitch, volume, filter cutoff/reso, drive, ring, sub, punch, warp, FM "
             "amount and wave position are modulated PER SAMPLE from ANY source - crank an LFO into "
             "the audio range = real FM / AM / filter-FM timbres.\n"
             "- Attack / Decay / Sustain / Release are sampled ONCE PER HIT (a fast source scatters "
             "the hits, it never wobbles a running envelope; use the Volume target for continuous "
             "level movement). Engine knobs / detune / width / formant move at block rate, smoothed.\n"
             "- Sources include MOD WHEEL (CC1), AFTERTOUCH (per note on MPE controllers) and SLIDE "
             "(MPE CC74 by default; both wheel + slide are learnable to ANY CC via the MIDI dropdown)."; }
    juce::String routeSrcName(int r) const;
    juce::String routeTgtName(int r) const;
private:
    juce::Rectangle<float> faderRect(int i) const;
    int  faderAt(juce::Point<float> p) const;
    void setAmtFromX(int r, float x);
    int  dragRoute = -1;
};

//==============================================================================
// A sliding on/off switch: green with the knob to the right when on, grey with
// the knob to the left when off. (Declared early so SlotEditor can hold one.)
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
// INTERACTIVE string controller for the Physical engine (a visual CONTROLLER like the
// env/EQ editors, not a decoration): the drawn pluck shape IS the parameters. Drag the
// dot: X = strike Position (the comb), Y = Tone (brightness = how high the pluck sits).
// The string's jaggedness derives from Stiffness, its colour character from Material.
// Static unless a control changes - nothing animates on its own.
class StringDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<DrumChannel::Slot*()> getSlot;
    std::function<void()> onEdit;      // wrote Position/Tone (mark DSP dirty)
    std::function<void()> onDragEnd;   // released (auto-audition)
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
private:
    bool dragging = false;
};

//==============================================================================
// INTERACTIVE mallet controller for the Modal engine: a silhouette of the struck body
// (bar / tube / circle / plate / block per Material) with a draggable MALLET that travels
// the FULL width (a struck body is symmetric: both edges = edge strike, the middle =
// centre strike, so hit = 1 - |2x - 1|). Vibration arcs around the strike point show how
// freely it rings; dragging DOWN shortens them = Damp. Same family as the env editors.
class ModalDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<DrumChannel::Slot*()> getSlot;
    std::function<void()> onEdit;
    std::function<void()> onDragEnd;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
private:
    bool  dragging = false;
    float dotX = -1.0f;   // mallet x (0..1, FULL width); reconciled with the slot's hit on paint
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
//==============================================================================
// A compact horizontal DRAG-FADER in the Arp Rate-fader style: a rounded box, a coloured fill up
// to the thumb, "Name  value" centred/left inside. Click or drag maps X absolutely to 0..1.
// Reused for the per-slot FX (Drive / Reverb / Delay sends), the Chorus (Mix / Rate / Depth), the
// filter Keytrack and the LFO tempo-Sync. The accent colour follows the edited slot (yellow / pink).
class SlotDragFader : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void(float)> onChange;             // value 0..1 (the handler maps to the real range)
    std::function<void()>      onDragEnd;
    std::function<juce::String(float)> format;       // 0..1 -> display text (e.g. "42%", "Off", "2 /bar")
    void setAccent(juce::Colour c)  { if (c != accent_) { accent_ = c; repaint(); } }
    void setValue01(float v)        { v = juce::jlimit(0.0f, 1.0f, v); if (std::abs(v - val_) > 1.0e-4f) { val_ = v; repaint(); } }
    float modRing_ = -1.0f;         // live modulated position 0..1 (a cyan marker); -1 = none
    void setModRing(float v)        { if (std::abs(v - modRing_) > 0.004f) { modRing_ = v; repaint(); } }
    void setLabel(const juce::String& s) { if (s != name_) { name_ = s; repaint(); } }
    void setDefault(float d)        { dflt_ = juce::jlimit(0.0f, 1.0f, d); }
    void setVertical(bool v)        { vertical_ = v; }   // vertical fills bottom->top; drag maps Y
    float value01() const           { return val_; }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r, 4.0f);
        g.setColour(accent_.withAlpha(0.30f));
        if (vertical_) g.fillRoundedRectangle(r.withTop(r.getBottom() - juce::jmax(5.0f, val_ * r.getHeight())), 4.0f);
        else           g.fillRoundedRectangle(r.withWidth(juce::jmax(5.0f, val_ * r.getWidth())), 4.0f);
        g.setColour(accent_.withAlpha(0.55f)); g.drawRoundedRectangle(r, 4.0f, 1.0f);
        if (modRing_ >= 0.0f)   // live modulated position = a cyan marker line (across the fader)
        { const float p = juce::jlimit(0.0f, 1.0f, modRing_);
          g.setColour(juce::Colour(0xff35c0ff));
          if (vertical_) { const float my = r.getBottom() - p * r.getHeight();
                           g.fillRect(r.getX() + 1.0f, my - 1.0f, r.getWidth() - 2.0f, 2.0f); }
          else           { const float mx = r.getX() + p * r.getWidth();
                           g.fillRect(mx - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f); } }
        g.setColour(juce::Colour(0xffeaf0fa));
        juce::String t = name_.isEmpty() ? (format ? format(val_) : juce::String())   // value-only (name in a Label below)
                                         : (format ? name_ + "  " + format(val_) : name_);
        {   // [2026-07-15 17:00] ADAPTIVE font: shrink until the text FITS the fader's long axis
            // (squeeze-never-clip - "50% -6.0dB" was losing its "dB" at the fixed 11.5px).
            const int room = (vertical_ ? getHeight() : getWidth()) - 8;
            float fs = 11.5f;
            while (fs > 8.0f && juce::GlyphArrangement::getStringWidthInt(juce::Font(fs, juce::Font::bold), t) > room)
                fs -= 0.5f;
            g.setFont(juce::Font(fs, juce::Font::bold));
        }
        if (vertical_)
        {   // draw the label rotated 90 deg so it reads up the narrow column
            juce::Graphics::ScopedSaveState ss(g);
            g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi,
                                                           (float) getWidth() * 0.5f, (float) getHeight() * 0.5f));
            const int hw = getHeight(), hh = getWidth();
            g.drawText(t, (getWidth() - hw) / 2, (getHeight() - hh) / 2, hw, hh, juce::Justification::centred, false);
        }
        else g.drawText(t, getLocalBounds().reduced(7, 0), juce::Justification::centredLeft, false);
    }
    MidiLearnManager* mlm = nullptr;                 // right-click MIDI-learn (only faders GIVEN a pid)
    juce::String learnPid;
    std::function<bool()> onRightClick;              // custom right-click (bus menus); true = handled
    void mouseDown(const juce::MouseEvent& e) override
    { if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
      { if (onRightClick && onRightClick()) return;
        if (mlm != nullptr && learnPid.isNotEmpty()) showMidiLearnMenu(this, *mlm, learnPid, -1);
        return; }
      drag_ = true; lastDragPos_ = vertical_ ? e.position.y : e.position.x;
      if (! e.mods.isShiftDown()) apply(e); }             // SHIFT-press = fine mode arms without jumping
    void mouseDrag(const juce::MouseEvent& e) override
    {   // [2026-07-15 13:30] SHIFT = FINE DRAG (user): the value moves at 1/5 speed RELATIVE to the
        // press point instead of jumping to the absolute position - precision, not a snap toggle.
        const float pos = vertical_ ? e.position.y : e.position.x;
        if (e.mods.isShiftDown())
        {
            const float extent = juce::jmax(1.0f, (float) (vertical_ ? getHeight() : getWidth()));
            const float d01 = (pos - lastDragPos_) / extent * (vertical_ ? -1.0f : 1.0f);
            lastDragPos_ = pos;
            const float v = juce::jlimit(0.0f, 1.0f, val_ + d01 * 0.2f);
            if (std::abs(v - val_) > 1.0e-5f) { val_ = v; repaint(); if (onChange) onChange(val_); }
            return;
        }
        lastDragPos_ = pos;
        apply(e);
    }
    void mouseUp  (const juce::MouseEvent&)   override { if (drag_ && onDragEnd) onDragEnd(); drag_ = false; }
    void mouseDoubleClick(const juce::MouseEvent&) override
    { setValue01(dflt_); if (onChange) onChange(val_); if (onDragEnd) onDragEnd(); }
private:
    void apply(const juce::MouseEvent& e)
    {
        const float v = vertical_
            ? juce::jlimit(0.0f, 1.0f, 1.0f - (float) e.position.y / juce::jmax(1.0f, (float) getHeight()))
            : juce::jlimit(0.0f, 1.0f,        (float) e.position.x / juce::jmax(1.0f, (float) getWidth()));
        if (std::abs(v - val_) > 1.0e-4f) { val_ = v; repaint(); if (onChange) onChange(val_); }
    }
    bool vertical_ = false;
    juce::String name_;
    float val_ = 0.0f, dflt_ = 0.0f, lastDragPos_ = 0.0f;
    juce::Colour accent_ { 0xff35c0ff };
    bool drag_ = false;
};

//==============================================================================
// LfoDisplay: the per-slot LFOs as an INTERACTIVE visual (same family as the env/EQ editors -
// the drawn wave IS the parameters). THREE fully independent LFOs, one per target (FILT / PITCH /
// VOL) - the top tabs pick WHICH one you're editing (dragging never touches the other two), and
// any mix can run at once (active-but-unselected LFOs show as dim ghost waves). Drag LEFT/RIGHT =
// Rate (log 0.1..20 Hz, more cycles appear), UP/DOWN = Amount (wave height; 0 = off).
// Double-click = off / restore. A live dot rides the wave at the real playing voice's phase.
class LfoDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void(int dest, float rate, float amt)> onChange;
    std::function<void()> onDragEnd;
    std::function<void(int dest)> onDestChange;      // a tab was selected
    std::function<void(int dest, float sync)> onSyncChange;   // Sync mode/value edited (0 off / cpb / -1 grid)
    static constexpr int CVN = 64;   // = DrumChannel::Slot::LFO_CURVE_N (drawn LFO cycle points)
    void setValues(const float* rates, const float* amts, const float* syncs, const int* shapes,
                   const bool* frees, const bool* legs, const float (*curves)[CVN], const bool* routed,
                   bool filterOn, bool waveOn, juce::Colour accent)
    {
        bool ch = (filterOn != filtOn_) || (waveOn != waveOn_) || (accent != accent_);
        for (int d = 0; d < 4; ++d) { ch = ch || rates[d] != rate_[d] || amts[d] != amt_[d] || syncs[d] != sync_[d]
                                              || shapes[d] != shape_[d] || frees[d] != free_[d] || legs[d] != leg_[d]
                                              || routed[d] != routed_[d];
                                      rate_[d] = rates[d]; amt_[d] = amts[d]; sync_[d] = syncs[d];
                                      shape_[d] = shapes[d]; free_[d] = frees[d]; leg_[d] = legs[d]; routed_[d] = routed[d];
                                      for (int k = 0; k < CVN; ++k)
                                      { ch = ch || std::abs(curves[d][k] - curve_[d][k]) > 1.0e-4f;
                                        curve_[d][k] = curves[d][k]; } }
        filtOn_ = filterOn; waveOn_ = waveOn; accent_ = accent; if (ch) repaint();
    }
    std::function<void(int dest)> onOpenCurveEditor;   // Custom: a plain CLICK on the wave opens the draw window
    std::function<void(int dest, int shape)> onShapeChange;   // Shape button cycled
    std::function<void(int dest, bool freeRun, bool legato)> onTrigChange; // Retrig/Free/Legato cycled
    std::function<void(float a, float h, float d, float s, float r)> onModEnvChange;  // TAB 4 = Mod Env A-H-D-S-R dragged
    void setModEnv(float a, float h, float d, float s, float r)
    { if (a != modEnvA_ || h != modEnvH_ || d != modEnvD_ || s != modEnvS_ || r != modEnvR_)
      { modEnvA_ = a; modEnvH_ = h; modEnvD_ = d; modEnvS_ = s; modEnvR_ = r; if (dest_ == 3) repaint(); } }
    void setFreeClockSec(double s) { if (std::abs(s - freeSec_) > 1.0e-4) { freeSec_ = s;
        if (free_[dest_] && amt_[dest_] > 0.001f) repaint(); } }   // dot keeps moving between notes
    // Live tempo so the drawn wave + read-out show the TRUE synced speed (never the ignored
    // free-Hz value - the honesty rule). (Grid mode + its gridCpb are GONE [2026-07-15 14:20].)
    void setTempoInfo(float barSec)
    { barSec = juce::jmax(0.05f, barSec);
      if (std::abs(barSec - barSec_) > 1e-4f) { barSec_ = barSec; repaint(); } }
    int  selDest() const { return dest_; }   // the tab being edited (the timer feeds ITS phase)
    void setFilterOn(bool on) { if (on != filtOn_) { filtOn_ = on; repaint(); } }   // live update of the FILT-off warning
    void setPhase(double ph) { if (std::abs(ph - phase_) > 1.0e-3) { phase_ = ph; repaint(); } }
    void setLiveCycle(uint32_t c2) { if (c2 != liveCyc_) { liveCyc_ = c2; if (shape_[dest_] == 4) repaint(); } }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent& e) override
    { if (dest_ == 3) { if (envDrag_ >= 0 && onDragEnd) onDragEnd(); envDrag_ = -1; return; }   // Mod Env handle released
      const bool wasDrag = dragging_; dragging_ = false;
      if (wasDrag && ! dnMoved_ && shape_[dest_] == 7 && waveArea().contains(e.position))
      { if (onOpenCurveEditor) onOpenCurveEditor(dest_); return; }   // plain click in Custom = open the draw window
      if (wasDrag && dnMoved_ && onDragEnd) onDragEnd(); }
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
private:
    float rate_[4] = { 4.0f, 4.0f, 4.0f, 4.0f }, amt_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float sync_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // per-dest tempo sync: 0 = off (free Hz), > 0 = cycles/bar, -1 = grid
    int   shape_[4] = {};                          // 0 Sine .. 7 Custom (the drawn curve)
    bool  free_[4]  = {};                          // FREE-RUN (timeline-anchored) vs RETRIG per note
    bool  leg_[4]   = {};                          // LEGATO: overlapping notes inherit the phase
    bool  routed_[4] = {};                         // does LFO d have a mod-matrix route? (for the "not routed" hint)
    float curve_[4][CVN] = {};                     // shape 7: the drawn cycle (-1..1)
    bool  dnMoved_ = false;                        // click vs drag (a plain click in Custom opens the editor)
    double freeSec_ = 0.0;                         // live free-run clock (editor timer)
    uint32_t liveCyc_ = 0;                         // the playing note's S&H cycle base (Random draws ITS pattern)
    float barSec_ = 2.0f;                      // live tempo (setTempoInfo)
    int dest_ = 0; bool filtOn_ = false, waveOn_ = false;   // waveOn_ = the slot's Wave is Custom (WAVE dest audible)
    double phase_ = -1.0;
    juce::Colour accent_ { 0xffe8bf4d };
    float dnRate_ = 4.0f, dnAmt_ = 0.0f; bool dragging_ = false;
    int   dnSyncIdx_ = 0;                      // detent index at drag start (Sync mode X-snapping)
    float lastAmt_[4] = { 0.5f, 0.5f, 0.5f, 0.5f };  // restore values for the double-click off/on toggle
    juce::Point<float> dnPos_;
    // The wave's true speed in Hz for dest d (what the DSP actually plays - synced rates included).
    float effHz(int d) const { const float s = sync_[d];
        return s == 0.0f ? rate_[d]
             : s < 0.0f  ? 261.63f * rate_[d]                        // KEY: drawn at middle C x ratio (per-voice in the DSP)
             : s / barSec_; }                                        // Bar sync (GRID mode deleted [2026-07-15 14:20])
    // GENERIC LFO: the hue follows what the LFO AFFECTS (its assigned destination), so the wave
    // colour tells you its effect at a glance; Off = neutral grey.
    juce::Colour destCol(int lfoIdx) const;   // fixed per-LFO identity hue (impl in .cpp - needs kLfoDestCol)
    // Bottom = ONE button row [Shape][Retrig][Sync] + the speed read-out on its OWN line below
    // (user layout; the box grew into the spare strip under it).
    juce::Rectangle<float> shapeBtnRect() const { return { 4.0f,   (float) getHeight() - 31.0f, 56.0f, 13.0f }; }
    juce::Rectangle<float> freeBtnRect() const  { return { 64.0f,  (float) getHeight() - 31.0f, 52.0f, 13.0f }; }
    juce::Rectangle<float> syncBtnRect() const  { return { 120.0f, (float) getHeight() - 31.0f, 60.0f, 13.0f }; }
    juce::Rectangle<float> waveArea() const { return getLocalBounds().toFloat().reduced(2.0f).withTrimmedTop(17.0f).withTrimmedBottom(32.0f); }
    int destAt(juce::Point<float> p) const  // which of the 4 tabs (top strip: LFO 1/2/3 + Mod Env); -1 = none
    {
        const float w = (float) getWidth();
        if (p.y > 16.0f || p.x < w * 0.17f) return -1;   // below the strip / over the title
        return juce::jlimit(0, 3, (int) ((p.x - w * 0.17f) / (w * 0.2075f)));
    }
    // TAB 4 = Mod Env: a full A-H-D-S-R graph. Fixed visual bands (A .22 | H .16 | D .24 | Sus .16 |
    // R rest) keep the 4 handles apart in the small area; each handle's position within its band =
    // the (log/linear) time, and the decay handle's Y = the sustain level.
    float modEnvA_ = 0.005f, modEnvH_ = 0.0f, modEnvD_ = 0.30f, modEnvS_ = 0.0f, modEnvR_ = 0.10f;
    int   envDrag_ = -1;                // 0 attack / 1 hold / 2 decay+sustain / 3 release; -1 none
    juce::Rectangle<float> envRect() const { return waveArea().reduced(6.0f, 4.0f); }
    static float A2f(float a){ return std::log(juce::jlimit(0.001f,2.0f,a)/0.001f)/std::log(2.0f/0.001f); }
    static float f2A(float f){ return 0.001f*std::pow(2.0f/0.001f, juce::jlimit(0.0f,1.0f,f)); }
    static float D2f(float d){ return std::log(juce::jlimit(0.01f,4.0f,d)/0.01f)/std::log(4.0f/0.01f); }
    static float f2D(float f){ return 0.01f*std::pow(4.0f/0.01f, juce::jlimit(0.0f,1.0f,f)); }
    static float R2f(float r){ return r<=0.01f ? 0.0f : std::log(r/0.01f)/std::log(8.0f/0.01f); }
    static float f2R(float f){ return f<=0.001f ? 0.0f : 0.01f*std::pow(8.0f/0.01f, juce::jlimit(0.0f,1.0f,f)); }
    static constexpr float bA_=0.22f, bH_=0.16f, bD_=0.24f, bS_=0.16f;   // band widths (R = the remainder)
    void modEnvHandles(juce::Point<float>& pk, juce::Point<float>& hd,
                       juce::Point<float>& dc, juce::Point<float>& rl, float& plateauEndX) const
    {
        const auto r = envRect();
        const float x0 = r.getX(), w = r.getWidth(), top = r.getY(), bot = r.getBottom();
        pk = { x0 + A2f(modEnvA_) * bA_ * w, top };
        hd = { x0 + (bA_ + juce::jlimit(0.0f,1.0f, modEnvH_/2.0f) * bH_) * w, top };
        const float susY = bot - juce::jlimit(0.0f,1.0f, modEnvS_) * (bot - top);
        dc = { x0 + (bA_ + bH_ + D2f(modEnvD_) * bD_) * w, susY };
        plateauEndX = x0 + (bA_ + bH_ + bD_ + bS_) * w;
        rl = { plateauEndX + R2f(modEnvR_) * (1.0f - (bA_+bH_+bD_+bS_)) * w, bot };
    }
    void paintModEnv(juce::Graphics& g);
};

//==============================================================================
// KEYS: the on-screen piano panel. Covers the detail area RIGHT of the slot boxes (amp/EQ,
// pitch, FX + master columns) when the KEYS view is on; the slot boxes stay visible so the
// sound is still editable. MONO piano at the bottom (a held-note stack gives real mono-synth
// behaviour: releasing the top key falls back to the previous held key); a control strip on
// top: REC (+ mode), Takes, Quantize (= the channel's step count), Slot-2 transpose, and
// Sustain/Release for the selected slot (live for KEY voices only - see DrumChannel::keyAdsr).
// The EDITOR wires + refreshes everything; this class just owns the widgets/layout/painting.
// A piano that can TINT individual keys (chord/scale/slot notes light up while you play). Slot 1
// yellow, slot 2 pink, both = a blend. Fills are near-solid so a tinted BLACK key still reads clearly.
class TintKeyboard : public juce::MidiKeyboardComponent
{
public:
    TintKeyboard(juce::MidiKeyboardState& s, Orientation o) : juce::MidiKeyboardComponent(s, o) {}
    juce::Colour tint[128] = {};                          // transparent (alpha 0) = no highlight
    bool anyTint = false;
    bool dim[128] = {};                                   // KEY GUIDE: true = out-of-scale key, drawn dimmed
    bool anyDim = false;
    bool splitMark = false;                               // MERGE&SPLIT on: mark C4 = the split boundary
    void clearTints() { if (! anyTint) return; for (auto& c : tint) c = juce::Colour(0u); anyTint = false; repaint(); }
    void setTint(int midi, juce::Colour c) { if (midi >= 0 && midi < 128) { tint[midi] = c; anyTint = true; } }
    void clearDims() { if (! anyDim) return; for (auto& d : dim) d = false; anyDim = false; repaint(); }
    void setDim(int midi, bool d) { if (midi >= 0 && midi < 128) { dim[midi] = d; anyDim |= d; } }
protected:
    juce::String getWhiteNoteText(int) override { return {}; }   // base labels off - we label EVERY key ourselves
    void drawWhiteNote(int n, juce::Graphics& g, juce::Rectangle<float> area,
                       bool isDown, bool isOver, juce::Colour line, juce::Colour text) override;
    void drawBlackNote(int n, juce::Graphics& g, juce::Rectangle<float> area,
                       bool isDown, bool isOver, juce::Colour fill) override;
};

// ARP EDITOR: a COMPACT popup (hidden until you click the "Arp" button, so it costs no permanent
// space). Top bar = On + Rate + Notes(length). Below = a 2 x 6 grid of the 12 offset rows (notes
// 2..13). Each cell: click the UP/DOWN arrow (or wheel) = +/-1 semitone (-24..+24, 0 = unison);
// DRAG = fast set; DOUBLE-CLICK = 0 st; RIGHT-CLICK = REST (a gap, distinct from 0). A cell edit extends the
// length to include it. Chord/scale/glide apply per arp note.
class ArpEditor : public juce::Component, public juce::SettableTooltipClient
{
public:
    bool   on   = false;
    int8_t off[DrumChannel::ARP_ROWS];
    int    len  = 2;     // pattern length INCLUDING the root (1 + rows)
    int    sync = 8;     // the Notes/bar fader (7/8/9/10/11/13) - the BASE grid
    int    rate = 8;     // Rate multiplier index (11 decimal rates x0.25..x3); 8 = x2
    bool   align = true; // phase-lock to the bar grid while the transport plays
    bool   altStrum = false; // alternate strum direction per note (up, down, up... like strumming)
    bool   hold  = false;// latch: keep looping after the key is released (same key again = stop)
    float  gate  = 1.0f; // note length as a fraction of one arp step (1 = ring into the next note)
    juce::Component* clickIgnore = nullptr;   // the Arp button: its own click toggles the popup
    std::function<void()> onChange;    // push these back onto the channel (+ mark modified)
    ArpEditor() { for (auto& o : off) o = 0; off[0] = 12; }   // mirror of the channel defaults (0 st, not REST)
    ~ArpEditor() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;        // (un)hooks the click-outside closer
    juce::String getTooltip() override;       // PER-CONTROL tooltips (varies with the hovered region)
    static constexpr int UI_COLS = 6, UI_ROWS = 2, TOPH = 40;   // ONE compact header row (popup got wider instead)
private:
    // Clicking ANYWHERE outside the popup closes it, like a real dropdown (global mouse listener,
    // hooked only while visible; the Arp button is exempt - its own click toggles).
    struct Closer : juce::MouseListener
    {
        ArpEditor& ed; explicit Closer(ArpEditor& e) : ed(e) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! ed.isVisible()) return;
            const auto p = e.getScreenPosition();
            if (ed.getScreenBounds().contains(p)) return;
            if (ed.clickIgnore != nullptr && ed.clickIgnore->getScreenBounds().contains(p)) return;
            ed.setVisible(false);
        }
    };
    Closer closer { *this };
    bool closerHooked = false;
    int  dragCell = -1;
    bool dragFader = false, dragRate = false, dragGate = false;
    void bump(int i, int d)   // nudge row i by d semitones (a REST cell lands on 0 first), extend length
    { if (i < 0) return; int v = (off[i] == DrumChannel::ARP_REST) ? 0 : (int) off[i] + d;
      off[i] = (int8_t) juce::jlimit(-24, 24, v); if (i + 2 > len) len = i + 2; if (onChange) onChange(); repaint(); }
    // ONE header row, left group absolute + right group anchored to the right edge (all 26px tall):
    // ON | Rate x2 | Notes/bar x8 | Align bar | Hold | Gate 100% | < Last note N >
    juce::Rectangle<int> onRect()    const { return { 6, 7, 40, 26 }; }
    juce::Rectangle<int> rateRect()  const { return { 52, 7, 92, 26 }; }                    // drag-fader
    juce::Rectangle<int> faderRect() const { return { 150, 7, 112, 26 }; }                  // Notes/bar drag-fader (same style)
    juce::Rectangle<int> alignRect() const { return { getWidth() - 392, 7, 78, 26 }; }
    juce::Rectangle<int> holdRect()  const { return { getWidth() - 308, 7, 52, 26 }; }
    juce::Rectangle<int> altRect()   const { return { getWidth() - 476, 7, 78, 26 }; }
    juce::Rectangle<int> gateRect()  const { return { getWidth() - 250, 7, 92, 26 }; }
    juce::Rectangle<int> lenDnRect() const { return { getWidth() - 152, 7, 20, 26 }; }
    juce::Rectangle<int> lenValRect()const { return { getWidth() - 130, 7, 100, 26 }; }
    juce::Rectangle<int> lenUpRect() const { return { getWidth() - 28, 7, 20, 26 }; }
    juce::Rectangle<int> cellRect(int i) const
    { const int r = i / UI_COLS, c = i % UI_COLS;
      const int gw = getWidth(), gh = getHeight() - TOPH;
      const int cw = gw / UI_COLS, ch = gh / UI_ROWS;
      return { c * cw, TOPH + r * ch, cw, ch }; }
    int cellAt(juce::Point<int> p) const
    { if (p.y < TOPH) return -1; const int cw = getWidth() / UI_COLS, ch = (getHeight() - TOPH) / UI_ROWS;
      const int c = juce::jlimit(0, UI_COLS - 1, p.x / juce::jmax(1, cw));
      const int r = juce::jlimit(0, UI_ROWS - 1, (p.y - TOPH) / juce::jmax(1, ch));
      return r * UI_COLS + c; }
};

// SCALE box (above the keyboard): the diatonic-harmonizer controls moved here from the sound editor.
// Per SLOT (1 = yellow / 2 = pink chips): a Key button + two Arp-style DRAG-FADERS - "Notes xN" (chord
// size) and the scale itself ("Off / Major / ..."). Editing writes straight onto the slot's scale fields.
class ScaleBox : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct V { bool on = false; int type = 0, key = 0, count = 3; };
    V   v[2];
    int slot = 0;                                  // which slot the faders edit - a MIRROR of the editor's ONE slot selection (setShapeSlot)
    MidiLearnManager* mlm = nullptr;               // [2026-07-16] right-click a fader/key = MIDI-learn (ui_sel_scale*)
    std::function<void(int slot)> onChange;        // editor writes v[slot] back onto the channel slot
    std::function<void(int slot)> onSlotSelect;    // [2026-07-16] chip click -> setShapeSlot (ONE selection everywhere; the editor syncs `slot` back)
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
private:
    bool dragNotes = false, dragScale = false;
    juce::Rectangle<int> chipRect(int i) const { return { 52 + i * 22, 1, 20, 14 }; }
    juce::Rectangle<int> keyRect()   const { return { getWidth() - 58, 1, 54, 14 }; }
    juce::Rectangle<int> notesRect() const { return { 4, 19, getWidth() - 8, 25 }; }
    juce::Rectangle<int> scaleRect() const { return { 4, 47, getWidth() - 8, 25 }; }
};

// LIVE TUNER strip (its own box UNDER the ScaleBox - user placement): what the selected channel
// is tuned to. While a key is held it follows THAT note's actual sounding pitch; idle it shows
// the Base Freq (+ roll Tune). Pure arithmetic fed by the editor timer - zero DSP cost.
class TunerBox : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::String note;                             // e.g. "A1"
    int          cents = 0;                        // -50..+50 (deviation from that note)
    void paint(juce::Graphics&) override;
    juce::String getTooltip() override
    { return "LIVE TUNE: what this channel is tuned to (the Base Freq, the roll's Tune fader and "
             "the note you are holding all flow in). Dots left = flat, right = sharp (one per "
             "~12 cents); the ring turns green within +-3 cents."; }
};

// SPLIT KEYBOARD control: a toggle + two Arp-style boxes. Each box = the full C0..C8 range; the
// INNER box = that slot's 4-OCTAVE WINDOW (drag it; octave-snapped; label = "C1-C5"). With Split ON,
// keys LEFT of middle C play SLOT 2 only (mapped into its window), keys RIGHT of it SLOT 1 only.
// MERGE & SPLIT control: a big two-line button (popup: merge with previous/next, or un-merge) + two
// STACKED window boxes. Merging two ADJACENT channels splits the keyboard BETWEEN them: keys at/above
// middle C play the pair's FIRST (lower-numbered) channel, below it the SECOND - each half mapped onto
// its 4-octave window (whole box = C0..C8; drag the inner box, octave-snapped; labels show the range).
class KeySplitBox : public juce::Component, public juce::SettableTooltipClient
{
public:
    int  chFirst = -1, chSecond = -1;         // the merged pair (channel indices; -1 = not merged)
    int  w1 = 60, w2 = 12;                    // window starts (MIDI, 12..60, octave-snapped)
    std::function<void()> onChange;           // window edits (written to the FIRST channel, all patterns)
    std::function<void()> onMergeClick;       // the big button -> merge/un-merge popup
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
private:
    bool merged() const { return chFirst >= 0 && chSecond >= 0; }
    int  dragBox = -1;                        // 0 = first/right window, 1 = second/left window
    juce::Rectangle<int> onRect() const  { return { 0, 1, 62, getHeight() - 2 }; }   // the tall Merge&Split button
    juce::Rectangle<int> boxRect(int i) const
    { const int hh = (getHeight() - 2) / 2; return { 68, 1 + i * (hh + 1), 148, hh - 1 }; }
    void dragTo(int i, int x);
};

class KeysPanel : public juce::Component, private juce::MidiKeyboardState::Listener
{
public:
    explicit KeysPanel(MidiLearnManager& mlm);            // knobs + REC are MIDI-learnable (ui_sel_*)
    ~KeysPanel() override { kbState.removeListener(this); }
    std::function<void(int note, float vel)> onKeyDown;   // -> processor (mono handled here)
    std::function<void(int)> onKeyUp;   // which note was released (slide-safe mono pairing)

    LearnableButton  btnRec   { "REC" };                  // paramId "ui_sel_rec" (set in ctor)
    juce::ComboBox   comboRecMode;
    juce::TextButton btnSlot2 { "0 st" };                 // slot-2 transpose (3-column popup, -24..+24)
    juce::TextButton btnTakes { "Takes (0)" };
    // The feel knobs are SELECTED-SCOPE MIDI targets (right-click = learn; they act on the
    // selected pattern's selected channel via the SelCC ring).
    LearnableKnob humanKnob, strumKnob, minVelKnob, maxVelKnob, glideKnob;   // SLOT OFFSET / STRUM / min+max vel / GLIDE
    juce::TextButton btnPlayMode { "Poly" };              // [2026-07-16] PLAY MODE dropdown: Poly / Poly Legato / Mono / Mono Legato (legato = envelope continues; Glide = separate knob)
    juce::TextButton btnArp { "Arp" };                    // opens the ARP editor popup (space-saving)
    juce::TextButton btnGuide { "Guide" };                // KEY GUIDE popup: dim out-of-scale keys (display only)
    juce::Label      lblGuideCur;                         // caption under it: the active key + scale ("Off")
    ScaleBox         scaleBox;                            // per-slot SCALE harmonizer controls (moved from the sound editor)
    TunerBox         tunerBox;                            // LIVE TUNE strip under the ScaleBox (user placement)
    KeySplitBox      splitBox;                            // SPLIT keyboard: L half = slot 2, R half = slot 1 (windowed)
    juce::Label      lblChord[3];                         // LIVE names: [0] slot 1 (yellow), [1] slot 2 (pink), [2] ALL = both combined
    ArpEditor        arpEditor;                            // hold one key -> programmed riff (per-step); hidden until btnArp
    juce::Label      lblRecMode, lblSlot2, lblHuman, lblStrum, lblMinVel, lblMaxVel, lblPoly, lblGlide;
    bool             polyMode = false;                    // mirror of the channel's keysPolyMode (routes note-offs)
    int countdown = 0;                                    // count-in ticks left (drawn as a big 3-2-1)

    // Keyboard highlight (driven by the editor timer from the currently held key + selected channel):
    void clearKeyTints()               { kb.clearTints(); }
    void setKeyTint(int midi, juce::Colour c) { kb.setTint(midi, c); }
    void applyKeyTints()               { kb.repaint(); }
    void clearKeyDims()                { kb.clearDims(); }
    void setKeyDim(int midi, bool d)   { kb.setDim(midi, d); }
    void setSplitMark(bool on)         { if (kb.splitMark != on) { kb.splitMark = on; kb.repaint(); } }

    void paint(juce::Graphics&) override;
    void resized() override;
private:
    juce::MidiKeyboardState     kbState;
    TintKeyboard                kb { kbState, juce::MidiKeyboardComponent::horizontalKeyboard };
    juce::Array<int> held;                                // mono note stack (message thread only)
    float lastVel = 0.8f;
    void handleNoteOn (juce::MidiKeyboardState*, int, int note, float vel) override;
    void handleNoteOff(juce::MidiKeyboardState*, int, int note, float) override;
};

class SlotEditor : public juce::Component,
                   public juce::FileDragAndDropTarget,  // drop an audio file anywhere on the box -> load it as a Sample
                   public juce::DragAndDropTarget       // drag the OTHER slot's box here = copy its settings
{
public:
    // MOD RINGS on the engine GRID knobs: v[i] = the live modulated value of knob i (-1000 = not modulated).
    void setGridModRings(const float* v, int n)
    { for (int i = 0; i < (int) knobs.size() && i < n; ++i)
        knobs[i]->setModRing(v[i] < -900.0f ? -1.0f : (float) knobs[i]->valueToProportionOfLength(v[i])); }
    // Warp fader ring + the LIVE WAVE PREVIEW (the drawing follows the modulated FM/Warp, not just the knobs).
    void setOscModLive(float warpRaw, float fmRaw, float ratioRaw, float syncRaw = -1000.0f, float bendRaw = -1000.0f)
    { if (warpFader != nullptr) warpFader->setModRing(warpRaw < -900.0f ? -1.0f : (float) warpFader->valueToProportionOfLength(warpRaw));
      if (syncFader != nullptr) syncFader->setModRing(syncRaw < -900.0f ? -1.0f : (float) syncFader->valueToProportionOfLength(syncRaw));
      if (bendFader != nullptr) bendFader->setModRing(bendRaw < -900.0f ? -1.0f : (float) bendFader->valueToProportionOfLength(bendRaw));
      morphView.setModLive(fmRaw, ratioRaw, warpRaw, syncRaw, bendRaw); }
    std::function<void(int srcSlot)> onCopyFromSlot;   // the other slot's box was dropped on this one
    // DRAG-COPY SOURCE (user: "drag from where im not controlling knobs/faders", no handle/text): the
    // box is draggable from any NON-CONTROL area - the bare background + the text labels. A knob/fader/
    // combo or an interactive visual keeps its OWN drag (turning it must not also pick up the slot). A
    // nested mouse listener watches presses on the box + all its children and starts the copy-drag only
    // when the press did NOT land on a control. Dropping on the OTHER slot (incl. an empty one) copies it.
    struct BoxDragger : public juce::MouseListener
    {
        juce::Component* ownerComp = nullptr; int slotIndex = 0;
        void mouseDrag(const juce::MouseEvent& e) override
        {
            auto* ec = e.eventComponent;
            const bool bare = (ec == ownerComp) || (dynamic_cast<juce::Label*>(ec) != nullptr);
            if (! bare || e.getDistanceFromDragStart() < 6) return;   // on a knob/fader/combo/visual: leave it be
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(ownerComp))
                if (! dnd->isDragAndDropActive())
                    dnd->startDragging("slotcopy:" + juce::String(slotIndex), ownerComp);
        }
    };
    BoxDragger boxDragger;
    bool isInterestedInDragSource(const SourceDetails& d) override
    { return d.description.toString().startsWith("slotcopy:")
             && d.description.toString().substring(9).getIntValue() != index; }
    void itemDragEnter(const SourceDetails&) override { slotDropOver = true;  repaint(); }
    void itemDragExit (const SourceDetails&) override { slotDropOver = false; repaint(); }
    void itemDropped  (const SourceDetails& d) override
    {
        slotDropOver = false; repaint();
        if (onCopyFromSlot) onCopyFromSlot(d.description.toString().substring(9).getIntValue());
    }
    bool slotDropOver = false;   // paint a green outline while the other slot hovers here
    std::function<void()> onSelect;   // click the box (bare area) -> make this the slot being edited
    void mouseDown(const juce::MouseEvent&) override { if (onSelect) onSelect(); }
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
    std::function<void()> onFreqModeToggled;       // Hz <-> note read-out toggled (editor refreshes the sibling slot)
    bool pendingRebake = false;                    // a reBake param was touched this drag
    // CLICKING a pitched-Freq value read-out toggles the session-wide Hz <-> NOTE mode. The
    // listener attaches to the slider's internal text Label (recreated by setTextBoxStyle, so
    // hookFreqReadouts() re-attaches after every place()).
    struct ReadoutClick : juce::MouseListener
    {
        std::function<void()> onClick;
        // Fire on mouse-DOWN: waiting for mouse-up and rejecting "dragged" clicks made the toggle
        // feel flaky (a few pixels of jitter during a click counted as a drag and ate it).
        void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
    };
    ReadoutClick freqReadoutClick;
    void hookFreqReadouts();
    // TRANSPOSE LOCK (per-sound): disables every pitched Freq fader (osc freqFader + generic "nHz"
    // knobs) + swaps their tooltip to say where to unlock. Set by the editor before pushValues.
    bool freqDisabled = false;   // true when this channel is in PIANO ROLL: Freq is locked to C3
    void applyFreqLock();
    WaveMorphDisplay morphView;                     // Analog/FM only: Wave A->B morph (replaces 2 knobs)
    WavetableDisplay waveView;                       // SrcWave only: the current wavetable waveform
    StringDisplay    physView;                       // Physical only: interactive string (Position/Tone on the visual)
    ModalDisplay     modalView;                      // Modal only: interactive struck-body (Hit Pos/Damp on the visual)
    ToggleSwitch     fmEnvSw;                        // SrcOsc only: FM Amount follows the amp envelope
    // Drop an audio file on the box -> the editor switches this slot to Sample + loads it.
    std::function<void(const juce::File&)> onFileDropped;
    bool fileDragOver = false;                       // paint a drop highlight
    static bool isAudioFile(const juce::String& path)
    {
        const juce::String e = juce::File(path).getFileExtension().toLowerCase();
        return e == ".wav" || e == ".aiff" || e == ".aif" || e == ".mp3" || e == ".flac" || e == ".ogg";
    }
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    { return onFileDropped != nullptr && files.size() > 0 && isAudioFile(files[0]); }
    void fileDragEnter(const juce::StringArray&, int, int) override { fileDragOver = true;  repaint(); }
    void fileDragExit (const juce::StringArray&) override           { fileDragOver = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override
    { fileDragOver = false; repaint(); if (onFileDropped && files.size() > 0) onFileDropped(juce::File(files[0])); }
    // SrcOsc only: the box is split into ANALOG (wave + Freq fader) / FM (Depth fader + Ratio/Feedback
    // knobs) / PHYSICAL (Reson fader + revealed knob row). Freq, Depth (FM amount) and Reson (resonator
    // amount) are each a horizontal fader leading its section, so each knob group is one clean row.
    std::unique_ptr<LearnableKnob> freqFader, depthFader;
    // SrcOsc wave pickers: From / To vertical faders flanking the wave (replace click-to-cycle) + a
    // horizontal Warp fader (phase skew / PWM) in the ANALOG section.
    std::unique_ptr<LearnableKnob> fromFader, toFader, warpFader;
    // [2026-07-18] the SHAPE TRIO's other two faders (right column beside the FM trio):
    // warpFader = Fold (renamed on screen), syncFader = hard sync, bendFader = CZ phase distortion.
    std::unique_ptr<LearnableKnob> syncFader, bendFader;

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
        for (auto* f : { freqFader.get(), depthFader.get(), fromFader.get(), warpFader.get(),
                         syncFader.get(), bendFader.get() })
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
    juce::Rectangle<int> fmLabelR, resLabelR, oscLabelR, warpLabelR, freqLabelR, syncLabelR, bendLabelR;
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
    std::function<void()> onShiftClick;             // MERGE toggle: glue this pattern onto the previous one
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
    MidiLearnManager* mlm = nullptr;   // RIGHT-CLICK = MIDI-learn menu (5 ui_sel_env* targets)
    void setValues(float a, float h, float d, float s, float r);  // load (no callback)
    void setPlayheads(const float* sec, int n);                   // live voice positions (seconds) -> moving dots
    void setEnabledLook(bool en) { if (en == enabledLook) return; enabledLook = en; repaint(); }  // grey out (samples have no AHDSR)
    // Physical/Modal: 2-handle Strike/Ring (no Hold). allowSus lets the Ring handle carry a
    // SUSTAIN level on its Y axis (Physical only: a held key/gated note keeps the string ringing).
    void setStrikeRing(bool sr, bool allowSus = false)
    { if (sr == strikeRing && allowSus == srSus) return; strikeRing = sr; srSus = allowSus; repaint(); }
    // Samples don't gate -> their editor hides the meaningless Sustain/Release entirely.
    void setSusRelVisible(bool v) { if (v == ! noSusRel) return; noSusRel = ! v; repaint(); }
    void setNa(const juce::String& main, const juce::String& sub) { if (main == naMain && sub == naSub) return; naMain = main; naSub = sub; repaint(); }  // greyed-state message
    // Sample slots: the amp env is OPT-IN. When toggleable, double-clicking the (greyed or active)
    // graph fires onToggleRequest so the editor can flip Slot::smpEnvOn.
    void setToggleable(bool t) { if (t == toggleable) return; toggleable = t; repaint(); }
    std::function<void()> onToggleRequest;
    std::function<void(float,float,float,float,float)> onChange;  // a,h,d,s,r on drag
    std::function<void()> onDragEnd;                              // released after editing (for auto-audition)

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;    // sample slots: toggle the opt-in envelope
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp(const juce::MouseEvent&) override { const bool ed = drag >= 0; drag = -1; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;

    static constexpr float maxA = 6.0f, maxH = 6.0f, maxD = 6.0f, maxR = 4.0f;   // A/H/D up to 6 s; Release up to 4 s (2 s forced abrupt string/bell fades - user)
    static constexpr float maxAStrike = 0.05f;   // Strike/Ring mode: attack (Strike) maxes at 50 ms (a strike is short)
    static constexpr float kMinHold = 14.0f;   // px the Hold handle sits right of Attack (so it's visible at hold=0)
    static constexpr float kSkew = 0.22f;      // <1 => lots of room / fine control at the low (ms) end (very slow start)
private:
    float atk = 0.01f, hld = 0.0f, dcy = 0.1f, sus = 0.0f, rel = 0.06f;
    bool  enabledLook = true;      // false for sample slots (no amp envelope)
    bool  toggleable  = false;     // sample slots: double-click toggles the opt-in envelope
    bool  strikeRing  = false;     // Physical engine: show a 2-handle Strike(attack)/Ring(decay) envelope, no Hold
    bool  srSus       = false;     // Strike/Ring WITH a sustain level on the Ring handle's Y (Physical)
    bool  noSusRel    = false;     // hide Sustain/Release (Sample slots - nothing to gate)
    juce::String naMain = "AMP ENVELOPE (n/a - sample)", naSub = "sample plays full length";  // greyed-state text
    int   drag = -1, hover = -1;   // handle index: 0=A 1=H 2=Decay(x)+Sustain(y) 3=Release, -1=none
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
    // CHANNEL VOLUME lives ON the strip meter (user design): a white drag HANDLE (0..125%,
    // unity notch at 100%), so the channel level finally has a visible control.
    // Drag = set - double-click = 100% - right-click = MIDI learn (addressed + selected).
    static constexpr float VOL_MAX = 2.0f;   // +6 dB boost ceiling [2026-07-15 19:45] (was 1.25 = +1.9 dB)
    // [2026-07-14 00:50] MIXER dB TAPER (user: "max volume for a huge range" - the old handle was
    // LINEAR IN AMPLITUDE, so its whole top half spanned ~6 dB). Now the travel is linear in dB:
    // 0 = off, 0..75% = -54..0 dB, 75% (the unity notch) ..100% = 0..+6 dB (gain 2.0).
    // [2026-07-15 19:45] boost quarter re-tuned +1.9 -> +6 dB (user option 2: the old headroom was
    // "a quarter of the fader that does nothing"). Every consumer maps through these two functions
    // (drag, paint, fill cap, CC), so all faces of the control agree.
    static float posToGain(float p)
    {
        p = juce::jlimit(0.0f, 1.0f, p);
        if (p <= 0.0001f) return 0.0f;
        const float dB = p <= 0.75f ? -54.0f + (p / 0.75f) * 54.0f
                                    : (p - 0.75f) / 0.25f * 6.0206f;
        return juce::jlimit(0.0f, VOL_MAX, std::pow(10.0f, dB / 20.0f));
    }
    static float gainToPos(float g)
    {
        if (g <= 0.0021f) return g <= 0.0001f ? 0.0f : 0.0021f;   // ~-54 dB floor
        const float dB = 20.0f * std::log10(juce::jlimit(0.0021f, VOL_MAX, g));
        return dB <= 0.0f ? (dB + 54.0f) / 54.0f * 0.75f
                          : 0.75f + dB / 6.0206f * 0.25f;
    }
    std::function<void(float)> onVolume;    // user dragged the handle (value 0..VOL_MAX)
    std::function<void()>      onVolLearn;  // right-click (the editor shows the learn menu)
    void setVol(float v)
    { v = juce::jlimit(0.0f, VOL_MAX, v); if (std::abs(v - vol) > 1.0e-4f) { vol = v; repaint(); } }
    void mouseDown(const juce::MouseEvent& e) override
    { if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) { if (onVolLearn) onVolLearn(); return; }
      apply(e); }
    void mouseDrag(const juce::MouseEvent& e) override
    { if (! (e.mods.isRightButtonDown() || e.mods.isPopupMenu())) apply(e); }
    void mouseDoubleClick(const juce::MouseEvent&) override
    { setVol(1.0f); if (onVolume) onVolume(1.0f); }
    void paint(juce::Graphics& g) override;
private:
    float vol = 1.0f;       // pushed from the channel each tick (setVol)
    void apply(const juce::MouseEvent& e)
    { setVol(posToGain(e.position.x / juce::jmax(1.0f, (float) getWidth())));   // dB taper [2026-07-14]
      if (onVolume) onVolume(vol); }
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
    MidiLearnManager* mlm = nullptr;     // RIGHT-CLICK = MIDI-learn menu (5 ui_sel_uni* targets)
    static constexpr int kMaxUni = 16;   // Osc cap (KS 6 / Modal 3 set via setMaxUni)
    void setValues(int unison, int scaleUnison, float detune, float vibrato, bool centre, int detuneMode, bool scaleOn, int scaleType, int scaleKey, float uniSpread = 0.0f, float driftIn = 0.0f);
    // DRIFT visual honesty: the DSP's real rolled per-voice detunes (cents) from the newest playing
    // voice - the drawn lines move with what actually just played (change-gated repaint).
    void setDriftLive(const float* cents, int n);
    // LIVE MODULATION: a cyan ring at each dot's modulated position (detune/vib/width/drift; -1 = no
    // active route) - so a param targeted in the matrix visibly moves, like the FX-knob mod rings.
    void setModLive(float det, float vib, float width, float drift, float uniCnt = -1000.0f);
    float modUniCnt = -1000.0f;   // [2026-07-14] live modulated unison COUNT (ring at the uni handle; -1000 = no route)
    void setSupport(bool uniSupported, bool vibSupported, juce::String naReason);
    void setMaxUni(int m) { const int c = juce::jlimit(1, kMaxUni, m); if (c == maxUni) return; maxUni = c; if (uni > maxUni) uni = maxUni; if (uniScale > maxUni) uniScale = maxUni; repaint(); }  // per-engine unison cap
    std::function<void(int unison, float detune, float vibrato, bool centre, int detuneMode, bool scaleOn, int scaleType, int scaleKey, float uniSpread, float drift)> onChange;
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
    int   uni = 1;         // STD-mode voice count
    int   uniScale = 3;   // SCALE-mode voice count (chord size for the diatonic harmonizer)
    int   curUni() const { return scaleOn ? uniScale : uni; }   // the ACTIVE mode's count
    float det = 0.0f, vib = 0.0f;
    float uniWidth = 0.0f;   // stereo WIDTH of the unison/chord voices (top-left mini bar; NOT 'spread' - paint() has a pixel local by that name)
    float driftAmt = 0.0f;   // DRIFT: per-note randomness (phase scatter + micro-detune + level breath)
    float driftLive[17] = {}; int driftLiveN = 0;   // the last hit's REAL rolled detunes (cents)
    float modLive[4] = { -1.0f, -1.0f, -1.0f, -1.0f };   // live modulated detune/vib/width/drift (-1 = no active route)
    bool  centre = false;          // also play the original/undetuned pitch (toggled by double-click on Detune)
    int   mode = 0;                // detune direction: 0 = symmetric (drag right), 1 = up (drag up), 2 = down (drag down)
    bool  uniOn = true, vibOn = true;
    int   maxUni = 7;              // per-engine unison cap (Osc 7 / Modal 4 / Physical 3)
    bool  emitScaleOn = false; int emitScaleType = 0, emitScaleKey = 0;   // pass-through for emit()
    bool  scaleOn = false;         // SCALE (diatonic harmonizer) mode; the detune dot picks the scale type
    int   scaleType = 0;           // 0-9 which scale (Major, Minor, ...)
    int   scaleKey = 0;            // 0-11 key root pitch class (C = 0)
    juce::Rectangle<float> chip[4];   // [0]=STD [1]=CHORD [2]=SCALE toggle chips, [3]=KEY (shown only in SCALE mode)
    juce::String reason;
    int   drag = -1, hover = -1;   // 0 = Unison, 1 = Detune, 2 = Vibrato
    int   chipHover = -1;          // which mode chip the mouse is over (0=UNISON 1=CHORD 2=SCALE 3=KEY) for per-chip tooltips
    struct Geo { float left, right, top, bottom, cy, hh, uX, dX, vX, wX, xX;   // xX = DRIFT handle
                 float rangeX, rangeY, dPtX, dPtY, rootY, upRange, uniTop; };   // rootY = root line; upRange = room above it; uniTop = unison-dot ceiling (below the chips)
    Geo  geom() const;
    int  nearestHandle(juce::Point<float> p) const;
    void emit() { if (onChange) onChange(curUni(), det, vib, centre, mode, emitScaleOn, emitScaleType, emitScaleKey, uniWidth, driftAmt); }
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
    // Point the display at a channel's EQ bands (drawn + dragged in place) + the channel's
    // resonant FILTER (drawn/edited here too when showFilt - i.e. on the "All"/channel target).
    // TWO independent per-slot filters (series). type = DrumChannel::FilterType.
    void setFilters(int t0, float c0, float r0, float e0, float g0,
                    int t1, float c1, float r1, float e1, float g1, double sr);
    std::function<void()> onEdit;                // after a drag/wheel/toggle -> updateDSP + hash
    std::function<void()> onDragEnd;             // released after editing (for auto-audition)
    std::function<void(int filterIdx, int type, float cutoff, float reso, float envAmt, float gainDb)> onFilterEdit;
    std::function<void(float)> onFilterDriveEdit;   // FILTER DRIVE drag-box (one amount, drives BOTH filters)
    void setFilterDrive(float v) { v = juce::jlimit(0.0f, 1.0f, v); if (std::abs(v - fDrive) > 1.0e-4f) { fDrive = v; repaint(); } }
    int  active() const { return activeFilt; }   // which of the 2 filters the keytrack fader edits (last touched)
    // [2026-07-16] CHANNEL chip mode: the pair edits the post-FX CHANNEL filter - per-voice
    // concepts (the envelope arrow) don't exist there, so the env handles are hidden + inert.
    bool chanMode = false;
    void setChanMode(bool m) { if (m != chanMode) { chanMode = m; repaint(); } }
    void setModCutoff(int fi, float hz) { fi = juce::jlimit(0, 1, fi);   // live modulation ring: the modulated cutoff
        if (std::abs(hz - modCutoff[fi]) > (hz > 0 ? hz * 0.01f : 0.5f)) { modCutoff[fi] = hz; repaint(); } }
    // [2026-07-14 00:30] live MODULATED reso + env-amount (user: "live visual pls"). -1000 = no route.
    void setModReso(int fi, float q) { fi = juce::jlimit(0, 1, fi);
        if (std::abs(q - modReso[fi]) > 0.01f) { modReso[fi] = q; repaint(); } }
    void setModEnvAmt(int fi, float v) { fi = juce::jlimit(0, 1, fi);
        if (std::abs(v - modEnvA[fi]) > 0.005f) { modEnvA[fi] = v; repaint(); } }
    static juce::Colour filtColour(int fi) { return fi == 0 ? juce::Colour(0xffff7a4a) : juce::Colour(0xff35c0ff); }  // F1 orange / F2 cyan
    void pushSpectrum(const float* mags, int n);
    void decayTick();                            // call on a timer to fade slowly
    void paint(juce::Graphics& g) override;
    void mouseDown   (const juce::MouseEvent& e) override;
    void mouseDrag   (const juce::MouseEvent& e) override;
    void mouseMove   (const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseExit   (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp     (const juce::MouseEvent&) override { const bool ed = drag >= 0 || driveDrag; drag = -1; driveDrag = false; repaint(); if (ed && onDragEnd) onDragEnd(); }
    juce::String getTooltip() override;

private:
    float fDrive = 0.0f;         // FILTER DRIVE (saturation inside both SVF loops; 0 = clean = bit-identical)
    bool  driveDrag = false;
    juce::Rectangle<float> driveRect() const { return { (float) getWidth() - 98.0f, 2.0f, 94.0f, 14.0f }; }
    int   fType[2] = { 0, 0 };   // NOLINT
    float fCutoff[2] = { 1000.0f, 1000.0f }, fReso[2] = { 0.707f, 0.707f }, fEnvAmt[2] = { 0.0f, 0.0f };
    float fGain[2] = { 6.0f, 6.0f };   // BELL only: bipolar boost/cut dB (the diamond's Y = this, on the dB axis)
    float modCutoff[2] = { -1.0f, -1.0f };        // live modulated cutoff Hz per filter (-1 = not modulated)
    float modReso[2]   = { -1000.0f, -1000.0f };  // live modulated resonance (route-gated)
    float modEnvA[2]   = { -1000.0f, -1000.0f };  // live modulated env amount (route-gated)
    int   activeFilt = 0;                         // which filter the diamond drag / keytrack edits (last touched)
    bool  showFilter = true;
    // Handle ids: diamond of filter fi = 100+fi ; envelope end handle = 102+fi.
    static int kFilt(int fi)    { return 100 + fi; }
    static int kFiltEnv(int fi) { return 102 + fi; }
    float resoToNorm(float q) const { return juce::jlimit(0.0f, 1.0f, std::log(juce::jmax(0.31f, q) / 0.3f) / std::log(12.0f / 0.3f)); }
    float normToReso(float n) const { return 0.3f * std::pow(12.0f / 0.3f, juce::jlimit(0.0f, 1.0f, n)); }
    float filtEnvEndHz(int fi) const { return juce::jlimit(20.0f, 20000.0f, fCutoff[fi] * std::pow(2.0f, fEnvAmt[fi] * 5.0f)); }
    juce::Point<float> filtPos(juce::Rectangle<float> a, int fi) const
    {   // BELL: the diamond sits AT its gain on the dB axis (an honest EQ handle); others: Y = resonance.
        if (fType[fi] == DrumChannel::Bell)
            return { xForFreq(a, fCutoff[fi]), yForDb(a, juce::jlimit(-kMaxDb, kMaxDb, fGain[fi])) };
        return { xForFreq(a, fCutoff[fi]), a.getBottom() - resoToNorm(fReso[fi]) * a.getHeight() * 0.85f - a.getHeight() * 0.06f }; }
    juce::Point<float> filtEnvPos(juce::Rectangle<float> a, int fi) const
    {
        auto m = filtPos(a, fi);
        if (std::abs(fEnvAmt[fi]) <= 0.02f)   // env 0: park beside the diamond so a sweep is always grabbable
            return { m.x + 20.0f <= a.getRight() - 6.0f ? m.x + 20.0f : m.x - 20.0f, m.y };
        return { xForFreq(a, filtEnvEndHz(fi)), m.y };
    }
    double sampleRate = 44100.0;
    float scope[scopeSize]  = {}; // spectrum outline (peak-hold for consistency)
    bool  hasSpectrum = false;
    int   drag = -1, hover = -1;  // filter handle id being dragged / hovered

    float xForFreq(juce::Rectangle<float> a, float f) const { return a.getX() + freqToNorm(f) * a.getWidth(); }
    float freqForX(juce::Rectangle<float> a, float x) const { return normToFreq((x - a.getX()) / juce::jmax(1.0f, a.getWidth())); }
    float yForDb (juce::Rectangle<float> a, float db) const { return a.getCentreY() - juce::jlimit(-1.0f, 1.0f, db / kMaxDb) * a.getHeight() * 0.5f; }
    float dbForY (juce::Rectangle<float> a, float y)  const { return juce::jlimit(-kMaxDb, kMaxDb, (a.getCentreY() - y) / (a.getHeight() * 0.5f) * kMaxDb); }
    int   nearestBand(juce::Point<float> p) const;
    int hoverBand(juce::Point<float> p) const;   // tight, no area fallback (tooltips/highlight) [2026-07-15 17:00]
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().withTrimmedTop(11.0f).reduced(6.0f, 4.0f); }
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
class WaveformDisplay : public juce::Component, public juce::SettableTooltipClient,
                        public juce::FileDragAndDropTarget   // drop an audio file straight onto the waveform
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
    // LIVE playhead cursor: the actual read position of the newest playing voice (fraction of the
    // buffer, -1 = none). Fed each editor timer tick - it's the real position, not an animation.
    void setPlayhead(float frac)
    { if (std::abs(frac - playhead) > 0.002f || (frac < 0.0f) != (playhead < 0.0f)) { playhead = frac; repaint(); } }
    // Called whenever the regions change (drag/clear). The editor writes them to the slot.
    std::function<void(int n, const float* lo, const float* hi)> onRegionsChange;
    // Drop an audio file on the waveform -> the editor loads it into this slot.
    std::function<void(const juce::File&)> onFileDropped;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { fileDragOver = true;  repaint(); }
    void fileDragExit (const juce::StringArray&) override           { fileDragOver = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

private:
    void emitRegions() { if (onRegionsChange) onRegionsChange(regN, regLo, regHi); }
    std::vector<float> pMin, pMax;
    int   regN = 0;                        // number of drawn regions
    float regLo[MAXREG] = {}, regHi[MAXREG] = {};
    float dragAnchor = 0.0f; int dragIdx = -1;   // the region being dragged out
    bool  selEnabled = false;
    float lengthSec = 0.0f;    // sample length (watermark)
    bool  reversed = false;    // draw a REV badge when the sample plays backwards
    float playhead = -1.0f;    // live play position (fraction; -1 = not playing)
    bool  fileDragOver = false;
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
// Plugin-wide TOOLTIP look: bigger font (14px) + wider wrap (440px) so the long structured tips
// (one-line WHAT, blank line, "- " bullet lines) actually read as paragraphs, not a wall of text.
struct TipLNF : juce::LookAndFeel_V4
{
    static juce::TextLayout layoutTip(const juce::String& text, juce::Colour colour)
    {
        juce::AttributedString a;
        a.setJustification(juce::Justification::topLeft);
        a.append(text, juce::Font(14.0f), colour);
        juce::TextLayout tl; tl.createLayout(a, 440.0f); return tl;
    }
    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override
    {
        const auto tl = layoutTip(text, juce::Colours::black);
        const int w = (int) tl.getWidth() + 18, hh = (int) tl.getHeight() + 14;
        return juce::Rectangle<int>(pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 24,
                                    pos.y > parent.getCentreY() ? pos.y - (hh + 6) : pos.y + 6, w, hh)
                 .constrainedWithin(parent);
    }
    void drawTooltip(juce::Graphics& g, const juce::String& text, int w, int hh) override
    {
        g.fillAll(findColour(juce::TooltipWindow::backgroundColourId));
        g.setColour(findColour(juce::TooltipWindow::outlineColourId));
        g.drawRect(juce::Rectangle<int>(w, hh), 1);
        layoutTip(text, findColour(juce::TooltipWindow::textColourId)).draw(g, juce::Rectangle<float>((float) w, (float) hh).reduced(9.0f, 7.0f));
    }
};

struct TinyButtonLNF : juce::LookAndFeel_V4
{
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(11.5f, juce::Font::bold); }
    int getTextButtonWidthToFitText(juce::TextButton&, int) override { return 0; }
};

// Tiny-button font PLUS a purple rounded outline - marks a button whose action is NOT plain
// per-step editing (the top-bar Influence button, so users notice it behaves differently).
struct PurpleOutlineLNF : juce::LookAndFeel_V4
{
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(11.5f, juce::Font::bold); }
    int getTextButtonWidthToFitText(juce::TextButton&, int) override { return 0; }
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                              bool over, bool down) override
    {
        juce::LookAndFeel_V4::drawButtonBackground(g, b, bg, over, down);
        g.setColour(juce::Colour(0xffb96bff).withAlpha(b.isEnabled() ? 0.95f : 0.4f));
        g.drawRoundedRectangle(b.getLocalBounds().toFloat().reduced(1.0f), 4.0f, 1.8f);
    }
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
// In-editor DROPDOWN LIST with PER-ITEM TOOLTIPS (juce::ComboBox popups can't do per-item tips -
// long-documented limitation). A content child like the Arp/Route popups: hover a row = that row's
// tooltip; click = pick + close; click anywhere else (or the plugin losing OS focus - the editor
// watchdog) = close. Anchored under its combo. One shared instance serves every TipCombo.
class TipList : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Item { int id; juce::String name, tip; };
    void openFor(juce::ComboBox* anchor, std::vector<Item> its, int curId, std::function<void(int)> pick)
    {
        anchor_ = anchor; items = std::move(its); cur = curId; onPick = std::move(pick); hover = -1;
        int w = 150; for (auto& it : items) w = juce::jmax(w, 16 + (int) juce::GlyphArrangement::getStringWidthInt(juce::Font(12.5f, juce::Font::bold), it.name));
        const int h = (int) items.size() * rh + 6;
        auto* par = getParentComponent();
        auto ab = par != nullptr ? par->getLocalArea(anchor_, anchor_->getLocalBounds()) : anchor_->getBounds();
        int x = ab.getX(), y = ab.getBottom() + 2;
        if (par != nullptr) { x = juce::jlimit(0, juce::jmax(0, par->getWidth() - w), x);
                              if (y + h > par->getHeight()) y = juce::jmax(0, ab.getY() - 2 - h); }   // open upward if needed
        setBounds(x, y, w, h);
        setVisible(true); toFront(false);
    }
    void close()
    { if (! isVisible()) return; setVisible(false);
      if (anchor_ != nullptr) anchor_->hidePopup();   // LATCH RULE: free the combo's menuActive or the next click is swallowed
      anchor_ = nullptr; }
    bool openForCombo(const juce::Component* c) const { return isVisible() && anchor_ == c; }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff181830));
        g.setColour(juce::Colour(0xff777fa8)); g.drawRect(getLocalBounds(), 1);
        for (int i = 0; i < (int) items.size(); ++i)
        {
            auto r = rowRect(i);
            const bool isCur = items[(size_t) i].id == cur;
            if (i == hover) { g.setColour(juce::Colour(0xff2c3454)); g.fillRect(r); }
            else if (isCur) { g.setColour(juce::Colour(0x33e8bf4d)); g.fillRect(r); }
            g.setColour(isCur ? juce::Colour(0xffffe9b0) : juce::Colours::white);
            g.setFont(juce::Font(12.5f, isCur ? juce::Font::bold : juce::Font::plain));
            g.drawText(items[(size_t) i].name, r.reduced(8, 0), juce::Justification::centredLeft, false);
        }
    }
    void mouseMove(const juce::MouseEvent& e) override { const int h = rowAt(e.position); if (h != hover) { hover = h; repaint(); } }
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown(const juce::MouseEvent& e) override
    { const int i = rowAt(e.position); if (i >= 0 && onPick) { auto cb = onPick; const int id = items[(size_t) i].id; close(); cb(id); } }
    juce::String getTooltip() override
    { const int i = rowAt(getMouseXYRelative().toFloat()); return i >= 0 ? items[(size_t) i].tip : juce::String(); }
    void visibilityChanged() override
    { auto& d = juce::Desktop::getInstance();
      if (isVisible()) d.addGlobalMouseListener(&closer); else d.removeGlobalMouseListener(&closer); }
    ~TipList() override { juce::Desktop::getInstance().removeGlobalMouseListener(&closer); }
private:
    static constexpr int rh = 19;
    juce::Rectangle<int> rowRect(int i) const { return { 1, 3 + i * rh, getWidth() - 2, rh }; }
    int rowAt(juce::Point<float> p) const
    { for (int i = 0; i < (int) items.size(); ++i) if (rowRect(i).toFloat().contains(p)) return i; return -1; }
    std::vector<Item> items; int cur = 0, hover = -1;
    juce::ComboBox* anchor_ = nullptr;
    std::function<void(int)> onPick;
    struct Closer : juce::MouseListener
    {
        TipList& o; explicit Closer(TipList& t) : o(t) {}
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (! o.isVisible()) return;
            const auto sp = e.getScreenPosition();
            if (o.getScreenBounds().contains(sp)) return;
            if (o.anchor_ != nullptr && o.anchor_->getScreenBounds().contains(sp)) return;   // toggle = the combo's job
            o.close();
        }
    } closer { *this };
};

// A ComboBox whose popup is the TipList above (per-item tooltips). Same latch rules as PickerCombo:
// never clear menuActive in showPopup; the list calls hidePopup() on close; a click while OUR list
// is open toggle-closes it.
struct TipCombo : juce::ComboBox
{
    std::function<void()> onOpenList;
    std::function<bool()> listOpen;
    std::function<void()> onToggleClose;
    void showPopup() override { if (onOpenList) onOpenList(); else juce::ComboBox::showPopup(); }
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (listOpen && listOpen()) { if (onToggleClose) onToggleClose(); return; }
        juce::ComboBox::mouseDown(e);
    }
};

// Compact ComboBox skin for NARROW combos (the CHANNEL FX type pickers): a SMALL 7px chevron instead
// of the stock wide arrow button, and the text SQUEEZES to fit (never "..." - user hates the dots).
struct TinyComboLNF : juce::LookAndFeel_V4
{
    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(0.5f);
        g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r, 3.0f);
        g.setColour(box.hasKeyboardFocus(false) ? juce::Colour(0xff9fd1ff) : juce::Colour(0xff4a4a6e));
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        const float cx = (float) width - 7.0f, cy = (float) height * 0.5f;   // tiny chevron, right edge
        juce::Path a; a.addTriangle(cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.5f);
        g.setColour(juce::Colour(0xffaebada)); g.fillPath(a);
    }
    void positionComboBoxText(juce::ComboBox& box, juce::Label& l) override
    {
        l.setBounds(4, 1, box.getWidth() - 15, box.getHeight() - 2);   // text gets everything but the chevron
        l.setFont(juce::Font(11.0f, juce::Font::bold));
        l.setMinimumHorizontalScale(0.6f);                             // squeeze, never "..."
        l.setJustificationType(juce::Justification::centredLeft);
    }
};

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
        if (textWidth > 0)   // min h-scale 0.5 = long labels ("Mono Legato") SQUEEZE, never "..." (plugin rule)
            g.drawFittedText(b.getButtonText(), leftIndent, yIndent, textWidth, b.getHeight() - yIndent * 2,
                             juce::Justification::centred, 1, 0.5f);
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
    // Dim everything behind the zoom panel so the other boxes (and the KEYS keyboard) don't show through.
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xf60c0c16)); }
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
    void paintOverChildren(juce::Graphics& g) override;   // selected-strip outline (above the strip meters)
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;   // wheel over strips/patterns scrolls them
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
    void contentWheel(juce::Point<int> pos, float deltaY);   // wheel over the channel strips / pattern row scrolls them
    bool keyPressed(const juce::KeyPress&) override;         // Cmd/Ctrl+Z = undo, Cmd+Shift+Z / Ctrl+Y = redo
    void parentHierarchyChanged() override;                  // standalone: enable the OS maximize [2026-07-15 19:45]
    void setChannelMerge(int a, int b);   // MERGE&SPLIT toggle for two adjacent channels (channel-wide)
    double lastContentWheelMs = 0.0;   // rate-limit so a fast wheel/trackpad flood doesn't rocket the scroll
    void paintStripOutline(juce::Graphics&);   // selected strip's red outline, ABOVE children (the meters)

    // Fixed design WIDTH; the design HEIGHT grows with the number of visible channel rows
    // (contentHeightPx, recomputed by setVisibleChannels). DESIGN_H is the 8-channel default.
    static constexpr int DESIGN_W = 1510;   // widened to fit the Keys toggle next to Drag MIDI
    // FX column split (relative to colTop, colH = 338): the per-slot FX box on top, the CHANNEL FX box
    // (Chorus/Flanger/Phaser/Comp - whole instrument) under it. Used by layoutContent + paintContent.
    static constexpr int FX_BOX_H  = 206;   // per-slot FX box height (knobs compressed for the 3rd FX row below)
    static constexpr int CHFX_TOP  = 212;   // CHANNEL FX header top (below the FX box)
    static constexpr int CHFX_BOX_H = 126;  // CHANNEL FX box height (212 + 126 = 338 = colH)
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
    static constexpr int NUM_ZOOM = 15;    // master + 2 slots + amp/eq + pitch + fx + MODULATION (+ legacy/hidden headers)
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
    StepMagnifierOverlay stepMagOverlay;   // top-most: redraws the magnified step above the top bar/strips
    CountdownOverlay countdownOverlay;     // top-most: big 3-2-1-GO! recording count-in over the whole canvas

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
        juce::ValueTree   state;   // the state TREE (no serialize/deserialize - fast undo/redo)
        juce::String      presetName;
        juce::int64       presetBaselineHash = 0;
        bool              presetModified = false;
    };
    juce::TextButton btnUndo { "Undo" }, btnRedo { "Redo" };
    std::vector<UndoEntry> undoStack, redoStack;
    static constexpr int kUndoMax = 24;
    juce::int64 lastUndoHash = 0;
    juce::int64 undoTickHash = 0;   // this tick's stateHash, reused by the modified-marker check
    int timerCounter = 0;   // 60 Hz tick counter; heavy per-project hashing runs every 3rd tick
    int  undoStableTicks = 0;
    bool undoDirty = false;
    bool applyingUndo = false;
    void pushUndoSnapshot();
    void commitUndoNow();   // force-commit the CURRENT state as an undo entry (before a destructive edit)
    void applyUndoState(const UndoEntry& e);
    void doUndo();
    void doRedo();
    juce::String pickerQuery[Sequencer::NUM_CHANNELS];   // each channel's sound-search text (kept until cleared)
    // MIDI sound browsing (ui_sound_next/prev CCs -> the SELECTED channel's Sound Bank pick):
    void stepSoundBank(int dir);              // previous/next sound in the picker's order (wraps)
    int  currentSoundPickId(int ch) const;    // the channel's current sound as a picker id (by mixName; 0 = none)
    juce::uint32 lastSoundStepMs = 0;         // step rate limit (one browse step per ~220 ms)
    // Selected-scope MIDI controls (ui_sel_*): apply one queued CC to the current selection.
    void applySelCC(int target, float v, bool& slotDirty, bool& keysDirty);
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
    juce::Label    lblPatternsBars { {}, "(Bars)" };   // under "Patterns" - a merged run = one multi-bar piece
    PatternButton  patternBtns[Sequencer::NUM_PATTERNS];
    juce::TextButton patModeBtn;   // opens the Loop/Stop/Go-to menu; shows a summary
    LearnableButton btnFollow { "Follow" };    // global toggle: view follows the playing pattern
                                               // (proc.followPlayback); MIDI = "ui_sel_follow"
    juce::TextButton btnClearPat { "Clear" };  // wipe the current pattern's steps/values back to default
    LearnableButton  btnInfluenceTop { "Infl" };  // arm step-influence for the SELECTED channel (moved off the strips)
    // All 16 channels + 32 patterns are ALWAYS active now (the old 8/16 + 16/32 count toggles are gone).
    // This button (next to HIDE SOUND EDITOR/KEYS) switches the VIEW between 8 rows (default) and all 16.
    LearnableButton btn16View { "16 CHANNELS VIEW" };  // MIDI = "ui_sel_view16" [2026-07-15 22:30]
    // [2026-07-15 19:45] OTHERS trim (user design): a spring-back +-6 dB fader that scales EVERY
    // channel's volume EXCEPT the selected one (current pattern) by EDITING their handles - you
    // watch them move; undo covers it. Each drag is relative to the volumes at grab time; the
    // pill re-centres on release (a gesture, not a stored setting). + a momentary full reset.
    SlotDragFader    othersVolF;
    LearnableButton btnVolReset { "VOL RESET" };       // MIDI = "ui_sel_volReset" [2026-07-15 22:30]
    float othersVolBase[Sequencer::NUM_CHANNELS] = {};
    bool  othersVolActive = false;
    bool  keepPickerOnLayout = false;   // [2026-07-15 22:10] a sound pick relayouts WITHOUT closing the picker
    juce::uint32 othersCcLastMs = 0;    // [2026-07-15 22:30] Others via CC: re-capture the base after a ~0.5 s pause
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
    juce::TextButton btnAudition { "Auto" };   // global toggle: knob edits auto-play a TEST hit (proc.auditionOnEdit)
    LearnableButton btnToggleDetail { "HIDE SOUND EDITOR/KEYS" };   // collapse/expand; MIDI = "ui_sel_editor" [2026-07-15 22:30]
    bool detailShown = true;                   // when false, only the sequencer is shown (window shrinks)
    void  refreshAuditionButton();
    // ==== KEYS view (on-screen piano). Radio with the sound editor: the KEYS button shows the
    // OTHER view's name. The panel covers everything right of the slot boxes. =================
    KeysPanel        keysPanel { proc.midiLearn };
    LearnableButton btnKeysView { "KEYS" };            // MIDI = "ui_sel_keysView" [2026-07-15 22:30]
    bool keysView = false;                     // session-only; the sound editor is the default view
    // Takes live on the PROCESSOR (proc.keysTakes - persisted with the state/preset). The editor
    // ASSEMBLES them from the audio thread's event log: a 0xFF marker = loop boundary = the take
    // so far is closed and the next loop starts a fresh one (parseKeysEvents, called from the
    // timer while recording + once more on stop).
    int  keysEvtCursor = 0;                    // how far into proc.keysEvts we've parsed
    std::vector<DrumSequencerProcessor::KeyEvt> keysPendingEvts;   // the take being played right now
    int  keysCountdownTicks = 0;               // 3 s count-in (180 ticks @ the 60 Hz UI timer)
    int  keysGoTicks = 0;                      // brief "GO!" flash after the count-in ends
    int  keysRecStartTakeCount = 0;            // #takes when REC was pushed -> load the last NEW one on stop
    int  keysUiHash = -999;                    // change detector for the cheap panel refresh
    bool keysRecWasPlaying = false;            // transport ran during this take -> stop = finalize
    void applyKeysView();
    void keysStartRecord();
    void keysStopRecord(bool finalize);
    void keysDeleteTake(int idx);
    void keysLoadTake(int idx);
    void drainDrawTake();   // pull a finished-loop draw lane from the handshake into a take
    int    keysLoadedTakeIdx = -1;    // the take currently loaded onto its channel (for save-to/save-as-new)
    juce::int64 keysLoadedTakeHash = 0;
    juce::int64 takeDataHash(const DrumSequencerProcessor::KeysTake& t) const;   // fingerprint a take's data
    DrumSequencerProcessor::KeysTake captureTakeFromChannel(int ch, int pat) const;  // snapshot the live channel as a take
    bool   keysTakeDirty(int idx) const;   // has the loaded take's channel been hand-edited?
    int    takePatternOf(const DrumSequencerProcessor::KeysTake& t) const;   // which pattern a take belongs to
    int    takesForPatChan(int pat, int ch) const;   // count of takes for one pattern+channel (20 cap each)                // write a take's notes onto its channel (view/play it)
    void parseKeysEvents();                    // drain the audio event log into takes (live)
    void refreshKeysPanel();
    void updateKeyboardHighlight();   // light up the keys the held note voices (slot 1 yellow / slot 2 pink)
    uint64_t keysHighlightMaskLo = ~0ULL, keysHighlightMaskHi = ~0ULL;   // last held-note mask tinted for (~0 = force first update)
    int keysHighlightArpNote = -2;   // last arp note tinted for (the highlight FOLLOWS the arp live; -2 = force)
    int rollPreviewNote = -1;        // drawing-audition note currently held via pushKeyDown (-1 = none)
    int kbGuideApplied = -1;         // last applied guide state (mode<<8|key<<4|scale; -1 = force recompute)
    void updateKeyboardGuide();      // dim the out-of-scale keys per the KEY GUIDE setting
    // Host-frozen detector: if processHeartbeat stops moving (~1 s), the host isn't sending us
    // audio - the Play tooltip flips to a "Not playing?" explanation (see timerCallback).
    uint32_t lastHeartbeat = 0; int heartbeatStaleTicks = 0; bool hostFrozen = false;
    // Step-grid edit-mode radio buttons (none selected = normal on/off steps).
    LearnableButton btnModeVel { "Vel" }, btnModeLen { "Gate" }, btnModePitch { "Pitch" }, btnModeProb { "Loop" }, btnModeRoll { "Roll" }, btnModePan { "Pan" }, btnModeNudge { "Nudge" };
    LearnableButton btnModeModA { "Mod A" }, btnModeModB { "Mod B" };   // STEP MOD lanes (drawable modulation sources)
    juce::Label      lblEditMode;
    void setStepEditMode(int mode);   // 0 normal, 1 vel, 2 pitch, 3 prob

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
    // The Sound Bank combo does NOT open a JUCE PopupMenu: clicking it opens the editor's
    // SoundPickerPanel (one dropdown with a LIVE search field - user spec). ComboBox is kept
    // for the display text + the id-based apply plumbing.
    struct PickerCombo : juce::ComboBox
    {
        // ComboBox's private menuActive latch is set before showPopup() and calls
        // showPopupIfNotActive from BOTH mouseDown and mouseUp. Rules that make the panel behave:
        // leave the latch SET while the panel is open (the mouseUp call must stay a no-op - clearing
        // it in showPopup made the panel open-then-close on one click), and the panel calls
        // hidePopup() on the combo when it CLOSES so the next click can open again.
        std::function<void()> onOpen;          // open the picker panel
        std::function<bool()> panelOpen;       // panel currently open for THIS combo?
        std::function<void()> onToggleClose;   // close it (click on the combo = toggle)
        MidiLearnManager* mlm = nullptr;       // RIGHT-CLICK = MIDI-learn sound browsing (ui_sound_*)
        void showCcMenu();                     // the 3 learnable targets (knob / next / prev)
        void showPopup() override { if (onOpen) onOpen(); else juce::ComboBox::showPopup(); }
        void mouseDown(const juce::MouseEvent& e) override
        {
            if ((e.mods.isRightButtonDown() || e.mods.isPopupMenu()) && mlm != nullptr) { showCcMenu(); return; }
            if (panelOpen && panelOpen()) { if (onToggleClose) onToggleClose(); return; }
            juce::ComboBox::mouseDown(e);
        }
    };
    struct ChannelStrip
    {
        NumDragButton numBtn;
        std::unique_ptr<LearnableButton> btnMute;
        std::unique_ptr<LearnableButton> btnSolo;
        PickerCombo      comboSound;     // the "Sound Bank" selector
        juce::TextButton btnTest { "TEST" };
        LearnableButton btnPoly { "OV" }; // overlap / polyphony (MIDI-learnable)
        LearnableButton btnInfluence { "I" }; // arm step-influence for this channel (MIDI-learnable)
        juce::ComboBox   comboSteps;
    };
    ChannelStrip strips[Sequencer::NUM_CHANNELS];

    // Level meters: one horizontal bar per channel strip + a stereo (L/R) master meter. The
    // ballistics (dB scaling, fast attack / slow release, peak-hold) live in timerCallback.
    LevelMeter stripMeter[Sequencer::NUM_CHANNELS];
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
    // Empty pids = NOT MIDI-learnable (the old p0_ch0_* routes went to UI-less legacy fields
    // and were removed - no-hidden-params rule; an empty pid suppresses the learn menu).
    LearnableKnob knobPitch   { "", proc.midiLearn };   // visible as sample "Crush"
    LearnableKnob knobVolume  { "", proc.midiLearn };   // channel volume's learn surface = the strip METER
    LearnableKnob knobPan     { "", proc.midiLearn };
    LearnableKnob knobSlices  { "", proc.midiLearn };   // sample slicing (1 = off)
    juce::Label   lblSlices;
    LearnableKnob knobStretch { "", proc.midiLearn };   // time-stretch (needs SoundTouch)
    juce::Label   lblStretch;
    LearnableKnob knobCutoff  { "", proc.midiLearn };
    LearnableKnob knobReso    { "", proc.midiLearn };
    LearnableKnob knobEnvAmt  { "", proc.midiLearn };
    LearnableKnob knobDrive   { "", proc.midiLearn };
    juce::ComboBox comboFilterType;
    TipCombo       comboDriveType;   // drive-type dropdown with PER-ITEM tooltips (TipList)


    juce::ComboBox comboOutput;      // per-channel routing (Main / Out 1..N / MIDI Out)
    juce::Label    lblOutput;
    static constexpr int kMidiOutId = DrumSequencerProcessor::NUM_AUX_OUTS + 2;  // combo id for "MIDI Out"
    juce::TextButton btnRoute { "Routing" };   // top-bar: open the per-channel routing overview
    void refreshRouting();           // recolour the channel strips + Route button by routing
    // FX row = SELECTED-SCOPE MIDI targets: the CC acts on whatever pattern/channel/slot is
    // selected (SelCC ring -> editor timer). The old p{P}_ch{C}_* ids routed to LEGACY
    // channel-level fields with no UI = "assigned but nothing moves".
    // (knobReverb/knobDelay retired - the sends are CHANNEL FX box faders now, pids ui_sel_fxRev/fxDel.)
    LearnableKnob knobSub     { "ui_sel_fxSub",     proc.midiLearn };   // per-slot SUB oscillator (octave below)
    LearnableKnob knobFormant { "ui_sel_fxFormant", proc.midiLearn };   // per-slot FORMANT vowel morph
    LearnableKnob knobPunch   { "ui_sel_fxPunch", proc.midiLearn };   // per-slot PUNCH transient shaper
    LearnableKnob knobRing    { "ui_sel_fxRing",    proc.midiLearn };   // per-slot RING modulator
    LearnableKnob knobRingHz  { "ui_sel_fxRingHz",  proc.midiLearn };   // RING carrier (Hz; hard left = track the note)
    LearnableKnob knobSlotPan { "ui_sel_slotPan",   proc.midiLearn };   // static SLOT PAN (placement; movement = Auto-Pan)
    // ---- CHANNEL FX box: Chorus / Flanger / Phaser / Comp act on the WHOLE channel (both slots
    //      combined), so they do NOT follow the slot selector. Vertical faders (MASTER style), one row.
    juce::Label   hdrChannelFx;
    // CHANNEL FX = TWO selectable effect slots (type combo + Amount + Character faders) + the channel
    // Reverb/Delay SEND faders (right-click a send fader = pick its bus A/B + MIDI-learn).
    TipCombo comboChFx[3];
    TipList  fxTypeList;           // shared per-item-tooltip dropdown (channel FX types + drive types)
    TinyComboLNF   tinyComboLNF;   // compact skin for the three type combos (cleared in teardown!)
    SlotDragFader  chFxAmtF[3], chFxChrF[3];
    SlotDragFader  sendRevF, sendDelF;
    juce::Label   lblSub, lblFormant, lblPunch, lblRing, lblRingHz, lblSlotPan;
    // REVERB MODE: clicking the "REVERB" header cycles Room -> Hall -> Plate -> Shimmer (whole
    // preset, like the other master flavour controls). A tiny MouseListener makes the Label clickable.
    // Two-row STEREO/MONO toggle button (the old label+tiny switch was unreadable - user): both words
    // stacked inside one button, the ACTIVE row bright, the other dim. Click anywhere = toggle.
    struct MonoToggle : juce::Component, public juce::SettableTooltipClient
    {
        bool mono = false;
        std::function<void(bool)> onChange;
        void mouseDown(const juce::MouseEvent&) override { mono = ! mono; if (onChange) onChange(mono); repaint(); }
        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(juce::Colour(0xff26264a)); g.fillRoundedRectangle(r, 4.0f);
            g.setColour(juce::Colour(0xff4a4a6e)); g.drawRoundedRectangle(r, 4.0f, 1.0f);
            auto top = getLocalBounds().removeFromTop(getHeight() / 2).toFloat();
            auto bot = getLocalBounds().removeFromBottom(getHeight() / 2).toFloat();
            auto row = [&](juce::Rectangle<float> rr, const char* txt, bool on) {
                if (on) { g.setColour(juce::Colour(0xffd9a13d).withAlpha(0.25f)); g.fillRoundedRectangle(rr.reduced(2.0f, 1.5f), 3.0f); }
                g.setColour(on ? juce::Colour(0xffffe9b0) : juce::Colour(0xff6a7290));
                g.setFont(juce::Font(10.5f, on ? juce::Font::bold : juce::Font::plain));
                g.drawText(txt, rr, juce::Justification::centred, false);
            };
            row(top, "STEREO", ! mono);
            row(bot, "MONO",   mono);
        }
    };
    MonoToggle monoToggle;
    struct HdrClick : juce::MouseListener
    { std::function<void()> fn; std::function<void(const juce::MouseEvent&)> fnE;
      void mouseDown(const juce::MouseEvent& e) override { if (fnE) fnE(e); else if (fn) fn(); } };
    HdrClick revModeClick, delayBusClick;
    bool masterBusB = false;               // which shared bus the MASTER reverb/delay rows EDIT (A or B)
    void refreshReverbModeHeader();
    void openLfoCurveEditor(int dest);   // LFO SHAPER overlay
    void openRoutePicker(int route);     // two-column source|target picker for one matrix fader
    HarmonicEditor harmEd;    // ADDITIVE draw-harmonics overlay (content child)
    LfoCurveEditor lfoCurveEd;             // LFO SHAPER overlay (draw the Custom LFO cycle)
    ModFaderMatrix  modFaders;             // 12 route faders (6x2) inline in the MODULATION box
    RoutePicker     routePicker;           // its right-click source|target chooser (overlay)
    RemapEditor     remapEd;               // [2026-07-14] the per-route REMAP curve overlay (opened from the picker)
    int            routePickRoute = -1;    // which fader the picker is currently editing
    int            lfoCurveEdDest = 0;     // which LFO tab the overlay edits
    int            harmEdSlot = 0;

    LearnableKnob knobReverbRoom { "global_reverbRoom", proc.midiLearn };
    LearnableKnob knobDelayTime  { "global_delayTime",  proc.midiLearn };
    LearnableKnob knobDelayFB    { "global_delayFB",    proc.midiLearn };
    LearnableKnob knobDelayWet   { "global_delayWet",   proc.midiLearn };   // delay return level (was a hidden fixed 0.3)

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
    SlotSelector     slotSelMod;                            // 1/2 picker in the MODULATION box (the MOD/LFO is per-slot)
    juce::Label      hdrAmpEnv, hdrEqBox, hdrVoice;   // box/section titles (row 1)
    // === PER-SLOT EQ (begin) - target picker: 0 = All (channel EQ), 1/2/3 = that slot's EQ ===
    SlotSelector     slotSelEq;
    int              eqEditTarget = 1;   // which SLOT's filter is shown (1 or 2; "All" was removed v1.3.5)
    void             refreshEqTarget();          // point freqDisplay at the chosen EQ
    // === PER-SLOT EQ (end) ===
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
    ToggleSwitch     swSmpPreserve[DrumChannel::NUM_SLOTS];    // Sample: ignore step/draw/key pitch (default on)
    juce::Label      lblSmpPreserve[DrumChannel::NUM_SLOTS];
    LearnableKnob    knobSpeed { "p0_ch0_speed", proc.midiLearn };
    juce::Label      lblSpeed;
    // Sample source: pitch offset + reverse (pitch/env/time reuse knobPitch/PEnvAmt/PEnvTime)
    LearnableKnob    knobSmpPOff { "p0_ch0_smpPOff", proc.midiLearn };
    juce::Label      lblSmpPOff;
    // Source interaction: Ring (multiply), Warp (cross-pitch FM), Morph (cross-filter).
    juce::Label      lblBlendTitle, lblBlendBot;
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
    juce::TextButton swDelaySync;       // [2026-07-15 12:10] single-row lit buttons (user - the
    juce::Label      lblDelaySync;      //  label+switch pairs cost 4 rows; the labels are now unused)
    juce::TextButton swDelayPingPong;
    juce::Label      lblDelayPingPong;
    LearnableKnob    knobMasterVol   { "global_masterVol",   proc.midiLearn };
    LearnableKnob    knobMasterLimit { "global_masterLimit", proc.midiLearn };
    LearnableKnob    knobMasterGlue  { "global_masterGlue",  proc.midiLearn };
    LearnableKnob    knobMasterTilt  { "global_masterTilt",  proc.midiLearn };
    LearnableKnob    knobMasterSat   { "global_masterSat",   proc.midiLearn };
    juce::Label      lblRevDecay, lblMasterVol, lblMasterLimit, lblMasterGlue, lblMasterTilt, lblMasterSat;
    juce::Label      lblCpu, lblRam;   // [2026-07-14 01:50] stacked CPU / RAM readout (right of DRAG MIDI)
    int              sysStatTicks = 0;
    // MASTER = a narrow 3/5-width strip now: every master KNOB is shown as a VERTICAL drag-fader
    // (value inside, rotated; name in the Label below). These are VISUAL PROXIES over the hidden
    // knobs above - they reuse each knob's range/skew/format/default/MIDI, so no master logic changed.
    static constexpr int NMVF = 12;
    SlotDragFader    masterVF[NMVF];
    // [2026-07-15 02:30] the SYNC/TRAIL/DUCK/CHARACTER batch (user-designed): DIRECT faders (no
    // hidden-knob proxy) writing MasterFX bus-aware, all patterns. The three "synced variant"
    // faders share bounds with their free proxies; refreshMasterSyncFaders() picks which shows.
    SlotDragFader    delayBarNF;            // synced Time = echoes-per-bar 1..21 (replaces masterVF[9] while Sync)
    SlotDragFader    delayTrailF;           // MAX TRAIL: hard echo-count cap (top = unlimited = default)
    SlotDragFader    delayDuckF;            // DUCK: echoes tuck under the dry mix, bloom in gaps
    SlotDragFader    delayCharF;            // CHARACTER: depth of the delay mode's flavour (inert on Digital)
    SlotDragFader    revDecBarsF;           // synced Decay = COUNTED quarter-bars (replaces masterVF[5] while Sync)
    SlotDragFader    revPreBarsF;           // synced Pre = COUNTED 64ths of a bar (replaces masterVF[7] while Sync)
    SlotDragFader    revGateF;              // GATE: counted 16ths synced / free ms unsynced (bottom = off)
    SlotDragFader    masterWidthF;          // [2026-07-15 12:10] master stereo width (bass-safe M/S)
    juce::Label      lblDelTrail, lblDelDuck, lblDelChar, lblRevGate, lblRevSyncT, lblMasterWidth;
    juce::TextButton swReverbSync;          // reverb Sync: single-row lit button (user) - bars/fractions vs free
    int              masterEstTick = 0;     // throttle for the live "~2.1 s" decay estimate repaint
    void refreshMasterSyncFaders();
    juce::Label      hdrModulation;         // "MODULATION" group header (holds the MOD/LFO visual for now)

    // (EQ knobs removed - the EQ is drawn/dragged on freqDisplay now.)
    juce::Label lblPit,lblVol,lblPan;
    juce::Label lblCutoff,lblReso,lblEnvAmt,lblDrive,lblRev,lblDel;
    juce::Label lblFiltType,lblDrvType;
    juce::Label lblRevRoom,lblDelTime,lblDelFB,lblDelWet;
    juce::Label hdrEq, hdrFilter, hdrDrive, hdrChan, hdrSend, hdrMaster;

    KnobLNF knobLNF;                       // value read-out under each knob
    TwoToneFaderLNF blendLNF;              // pink/yellow two-tone SOUND BLEND fader
    TinyButtonLNF tinyBtnLNF;              // small font for M/S/Ø/OV strip buttons
    PurpleOutlineLNF purpleOutlineLNF;     // purple-bordered tiny button (top-bar Influence)
    WideMenuLNF wideMenuLNF;               // 3-column popup for the sound-mix menu
    BigComboLNF bigComboLNF;               // larger font for the sample chooser
    LogoStepMeter logoMeter;              // live master-volume meter built into the logo step ramp
    juce::HyperlinkButton verLink;        // EMPTY tall click-area next to the logo -> opens the Releases page
    juce::Label      lblVersion;          // "v1.3.0" drawn at the TOP of verLink's click area
    juce::Label      lblCheckUpd;         // "Check / Updates" under the version (both share verLink's click area)
    DropButtonLNF dropBtnLNF;             // down-triangle for the play-mode + routing "dropdown" buttons
    IconButtonLNF iconBtnLNF;             // play / stop / undo / redo glyphs
    std::vector<LearnableKnob*> allKnobs;  // for clean LNF teardown
    TipLNF tipLNF;                                    // 14px/440px structured-tooltip look (cleared in teardown)
    // [2026-07-13 22:40] Tooltips-off = return NO tip. The old trick (appear-delay = 0x3FFFFFFF)
    // OVERFLOWED JUCE's uint32 uptime arithmetic on Macs awake > ~37 days - the compare wrapped and
    // tips showed INSTANTLY = "the toggle does nothing" (user bug). Gating the tip text is exact.
    struct GateableTooltipWindow : juce::TooltipWindow
    {
        using juce::TooltipWindow::TooltipWindow;
        const bool* gate = nullptr;
        juce::String getTipFor(juce::Component& c) override
        { return (gate != nullptr && ! *gate) ? juce::String() : juce::TooltipWindow::getTipFor(c); }
    };
    GateableTooltipWindow tooltipWindow { this, 700 };

    //-- Visuals
    FrequencyDisplay freqDisplay;
    LfoDisplay       lfoDisplay;     // per-slot LFO visual (FX box bottom; follows the FX slot selector)
    // Per-slot Drive amount as an Arp-style drag-fader (accent = the edited slot's colour).
    // (fxReverb/fxDelay/chorus* sibling faders were dead declarations - removed in the review sweep.)
    SlotDragFader fxDriveFader;
    // (The LFO Sync/Rate faders were REMOVED - tempo sync lives INSIDE the LFO visual now: its Sync
    //  button cycles Off/Sync/Grid and dragging the wave snaps through the musical rates.)
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
    void applySoundPickId(int channel, int id);   // the one dispatch behind combo ids + the picker panel
    void openSoundPicker(int channel);            // the searchable Sound Bank dropdown (SoundPickerPanel)
    std::unique_ptr<juce::Component> soundPicker; // created on first open; child of `content`
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
    void refreshDrawModeButtons();   // grey Len/Pitch/Roll for a draw-mode channel
    void selectPattern(int p);
    void copyPatternContent(int src, int dst);   // duplicate src's steps + per-pattern settings into dst
    // MERGED GROUPS: copy ONE channel's SOUND (mix: slots/env/EQ/FX/... - not steps/routing) between
    // patterns; the timer keeps every group member's sounds mirroring the edited pattern.
    void copyChannelSound(int srcPat, int dstPat, int ch);
    void syncMergedGroupSounds();
    int  lastStepCap = -1;   // last applied step-count cap (64 / group bars) for the dropdowns
    int  lastStepMenuKey[Sequencer::NUM_CHANNELS] = { };   // [1.5.0] change-gate for the per-bar step-count menus
    void rebuildStepMenu(int strip);                        // per-bar "Pattern N >" submenus when merged
    // MERGED-GROUP view: the grid hands out CONCAT step indices - resolve to the right bar's channel
    // (step is rewritten to the bar-local index). Not in a group = the current pattern's channel.
    DrumChannel& groupStepChannel(int ch, int& step);
    int  currentPattern() const { return proc.sequencer.currentPattern; }

    //-- Presets
    static juce::File getPresetsFolder();
    void rebuildPresetMenu();
    void handlePresetChange();
    void initPreset();             // clean default kit (120 BPM, 4/4, 8 steps, no steps)
    void syncAfterStateChange();   // push BPM/time-sig to the UI + full refresh
    void refreshPatternOptions();
    void refreshSwingLabel();
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
    // Ticks in a row that a popup menu has been open while NO window of ours has OS focus
    // (= the user clicked outside the plugin). Debounces the auto-dismiss of open dropdowns.
    int          outsideFocusTicks = 0;
    int          tunerTick = 0, tunerSilence = 0;   // REAL-tuner cadence (~10 Hz) + brief hold-over
    int          lastModalCount = 0;      // menu-transition grace: a JUST-opened menu must not be
    juce::Component* lastModalComp = nullptr;   // dismissed by the focus hand-off from the one
                                          // closing (identity compared only, never dereferenced)
    juce::String stripMixShown[Sequencer::NUM_CHANNELS]; // last text drawn (avoid per-tick repaint)
    juce::String presetShown;

    void updateKnobParamIds();
    void updateStripParamIds();
    void refreshDetailPanel();
    void updateFxFaders(const DrumChannel::Slot& sl);   // push FX/Chorus/Keytrack/Sync fader values + slot accent
    void refreshChannelStrips();
    void refreshPatternButtons();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumSequencerEditor)
};
