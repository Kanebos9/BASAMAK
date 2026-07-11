#include "PluginEditor.h"
#include "../plugin/FactoryContent.h"

// StepGridComponent - the step grid / piano-roll editor component, split out of the
// PluginEditor.cpp monolith. Its file-static geometry helpers (prHdr*/slotNoteColour) move
// with it (used only here). Callback-driven: no dependency on DrumSequencerEditor.

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
    const int y = (ch - firstRow) * rowH;    // map channel -> on-screen row (scroll offset)
    // MERGED GROUP: give each BAR an equal-width slot (like the piano roll's per-bar columns), then
    // split that slot by the bar's OWN step count. So bar boundaries LINE UP across channels even
    // when they have different step counts (a 15-step bar and an 8-step bar occupy the same width).
    if (grpBars > 1)
    {
        int b = 0;
        while (b + 1 < grpBars && step >= barStep0[ch][b + 1]) ++b;   // the bar this concat step is in
        const int barSteps = juce::jmax(1, barStep0[ch][b + 1] - barStep0[ch][b]);
        const int local    = step - barStep0[ch][b];
        const double barW = (double) getWidth() / (double) grpBars;
        const double x0   = (double) b * barW;
        const double cw   = barW / (double) barSteps;
        const int x = (int) (x0 + (double) local * cw);
        const int w = (int) (x0 + (double) (local + 1) * cw) - x;
        return { x, y, w, rowH };
    }
    int n = numSteps[ch];
    if (n <= 0) n = 1;
    float stepW = (float)getWidth() / (float)n;
    int x = (int)(step * stepW);
    int w = (int)((step + 1) * stepW) - x;
    return { x, y, w, rowH };
}

int  StepGridComponent::gridDiv() const { return drawGridDiv; }
void StepGridComponent::setGridDiv(int n) { drawGridDiv = juce::jlimit(0, 64, n); repaint(); }

void StepGridComponent::update(const Sequencer& seq, bool hasSolo)
{
    for (int i = 0; i < NCH; ++i) drawTune[i] = seq.patterns[seq.currentPattern].channels[i].drawTuneCents;
    currentPattern = seq.currentPattern;
    // MERGED GROUP: show all the group's bars SIDE BY SIDE (steps concatenated per channel, roll
    // spanning grpBars * DRAW_RES columns). The viewed pattern is always the group HEAD.
    const int gHead = seq.groupHead(seq.currentPattern);
    const int gEnd  = seq.groupEnd(seq.currentPattern);
    grpBars = juce::jlimit(1, GRP_MAX, gEnd - gHead + 1);
    const bool playingGroup = seq.isCurrentlyPlaying
                              && seq.groupHead(seq.playPattern) == gHead
                              && seq.playPattern - gHead < grpBars;
    for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
    {
        const auto& hc = seq.patterns[gHead].channels[ch];
        muted[ch]    = hc.mute;
        soloed[ch]   = hc.solo;
        midiOutCh[ch]= hc.midiOut;
        drawMode[ch] = hc.drawMode;
        dVel[ch] = hc.drawVel;
        // Concatenate the group's STEPS for this channel (equal cell widths; bars that no longer fit
        // in MAX_STEPS cells are not shown). barStep0 records where each bar begins.
        int tot = 0, filled = 0;
        for (int b = 0; b < grpBars; ++b)
        {
            const auto& c = seq.patterns[gHead + b].channels[ch];
            const int n = juce::jlimit(1, DrumChannel::MAX_STEPS, c.numSteps);
            if (tot + n > DrumChannel::MAX_STEPS) break;   // bars past 64 concat cells aren't shown
            barStep0[ch][b] = tot;
            for (int s = 0; s < n; ++s)
            {
                const int d = tot + s;
                steps[ch][d] = c.steps[s];
                vel[ch][d]   = c.stepVel[s];
                pit[ch][d]   = c.stepPitch[s];
                roll[ch][d]  = c.stepRoll[s];
                rollDec[ch][d] = c.stepRollDecay[s];
                noteLen[ch][d] = c.stepNoteLen[s];
                slide[ch][d]   = c.stepSlide[s];
                merge[ch][d]   = c.stepMerge[s];
                pan[ch][d]     = c.stepPan[s];
                nudge[ch][d]   = c.stepNudge[s];
                condLen[ch][d]  = c.stepCondLen[s];
                condMask[ch][d] = c.stepCondMask[s];
            }
            tot += n; ++filled;
        }
        for (int b = filled; b <= grpBars; ++b) barStep0[ch][b] = tot;   // unfitted bars pin to the end
        numSteps[ch] = tot;
        // Playhead in concat space: the playing BAR's local step + that bar's offset.
        playStep[ch] = -1;
        if (playingGroup)
        {
            const int pb = seq.playPattern - gHead;
            const auto& pc = seq.patterns[seq.playPattern].channels[ch];
            const int n = juce::jmax(1, pc.numSteps);
            playStep[ch] = barStep0[ch][pb] + (((int) (seq.barPos() * n)) % n);
        }
        else if (grpBars == 1)
            playStep[ch] = seq.getChannelStep(ch);
        // Concatenate the group's ROLL NOTES (offset each bar by b * DRAW_RES).
        if (hc.drawMode)
        {
            if (drawDragCh != ch && ! (drawMagCh == ch && prMode != 0))
            {   // don't clobber the mirror mid-gesture
                int nn = 0;
                for (int b = 0; b < grpBars; ++b)
                {
                    const auto& c = seq.patterns[gHead + b].channels[ch];
                    const int nc = juce::jlimit(0, DrumChannel::DRAW_MAX_NOTES, c.drawNoteCount);
                    for (int i = 0; i < nc && nn < MIR_MAX; ++i)
                    {
                        drawNotes[ch][nn] = c.drawNotes[i];
                        drawNotes[ch][nn].start = (int16_t) (drawNotes[ch][nn].start + b * DrumChannel::DRAW_RES);
                        ++nn;
                    }
                }
                drawNoteCount[ch] = nn;
            }
        }
        else if (drawMagCh == ch || drawDragCh == ch)   // channel left piano-roll mode -> tidy overlay/stroke state
        { if (drawMagCh == ch) drawMagCh = -1; if (drawDragCh == ch) drawDragCh = -1; drawReadSemi = -128; }
    }
    anySolo = hasSolo;
    curLoop = seq.loopCount;   // for the Prob-mode current-loop indicator
    playBarFrac = playingGroup ? ((float) (seq.playPattern - gHead) + (float) seq.barPos()) / (float) grpBars
                : (seq.currentPattern == seq.playPattern && seq.isCurrentlyPlaying) ? (float) seq.barPos() : 0.0f;
    repaint();
}

// Shared cell chrome: step number (bottom-right) + MIDI-learn highlight ring.
void StepGridComponent::paintCellExtras(juce::Graphics& g, int ch, int step, juce::Rectangle<int> rr,
                                        juce::Rectangle<float> r, bool isActive, bool isCurrent)
{
    if (rr.getWidth() > 22)
    {
        g.setColour((isActive || isCurrent) ? juce::Colours::black.withAlpha(0.5f)
                                            : juce::Colour(0xff444466));
        g.setFont(juce::Font(9.0f));
        g.drawText(juce::String(step + 1), r.toNearestInt().reduced(3),
                   juce::Justification::bottomRight, false);
    }
    if (midiLearn != nullptr && midiLearn->isLearning()
        && midiLearn->getLearningParam() == stepParamId(ch, step))
    {
        g.setColour(juce::Colour(0x55ffd23b)); g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colour(0xffffd23b)); g.drawRoundedRectangle(r, 4.0f, 2.2f);
    }
}

// One VALUE-mode cell. Factored out so the held-down cell can be re-drawn at 1.5x (the step
// magnifier) by the top-most overlay, on top of everything, with an outline.
void StepGridComponent::paintValueCell(juce::Graphics& g, int ch, int step, juce::Rectangle<int> rr, bool magnified)
{
    {
        auto r = rr.toFloat().reduced(3.5f, 2.0f);
        const bool isActive  = steps[ch][step];
        const bool isCurrent = (playStep[ch] == step);
        const bool isMerged  = merge[ch][step];
        {
                // Value mode: each cell is a fader (Velocity/Probability) or a bipolar bar centred
                // on 0 (Pitch/Pan). ACTIVE steps get a brighter bg + a green outline so they read
                // apart from OFF steps in EVERY mode - crucial for Pitch/Pan, where the bar shrinks
                // to nothing at value 0 and dimming alone was not enough to tell them apart.
                g.setColour(isActive ? juce::Colour(0xff26263c) : juce::Colour(0xff181826));
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(isActive ? juce::Colour(0xff2f9e57) : juce::Colour(0xff32324e));
                g.drawRoundedRectangle(r, 4.0f, isActive ? 1.3f : 0.5f);

                const float alpha = isActive ? 1.0f : 0.28f;
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
                    const float semis = juce::jlimit(-(float) DrumChannel::PITCH_RANGE, (float) DrumChannel::PITCH_RANGE, pit[ch][step]);
                    const float midY = r.getCentreY();
                    const float frac = semis / (float) DrumChannel::PITCH_RANGE;   // -1..1 (+/-48 st = C-1..C7 around C3)
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
                else if (editMode == ModeNudge)
                {
                    // Nudge = micro-timing: a bipolar HORIZONTAL bar from the centre - drag LEFT =
                    // the hit fires EARLY, RIGHT = LATE (up to half a step each way).
                    const float nd = juce::jlimit(-1.0f, 1.0f, nudge[ch][step]);
                    const float midX = r.getCentreX();
                    const float w = std::abs(nd) * (r.getWidth() * 0.5f);
                    juce::Rectangle<float> bar = nd >= 0
                        ? juce::Rectangle<float>(midX,     r.getY(), w, r.getHeight())
                        : juce::Rectangle<float>(midX - w, r.getY(), w, r.getHeight());
                    g.setColour(juce::Colour(0xffff9040).withAlpha(alpha));
                    g.fillRect(bar.reduced(0.0f, 1.0f));
                    g.setColour(juce::Colour(0x66ffffff));
                    g.drawVerticalLine((int) midX, r.getY(), r.getBottom());   // grid line
                    // Read-out in MILLISECONDS at the current tempo (max shift = half this step's span).
                    int stepsInBar = juce::jmax(1, numSteps[ch]);
                    if (grpBars > 1) { int b = 0; while (b + 1 < grpBars && step >= barStep0[ch][b + 1]) ++b;
                                       stepsInBar = juce::jmax(1, barStep0[ch][b + 1] - barStep0[ch][b]); }
                    const double ms = nd * 0.5 * (barMs / (double) stepsInBar);
                    txt = nd == 0.0f ? juce::String("0")
                                     : (ms > 0 ? "+" : "") + juce::String(juce::roundToInt(ms)) + "ms";
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
                else if (editMode == ModeLen)
                {
                    // GATE length: a piano-roll-style bar from the left. WIDTH = how long the hit
                    // sounds, as a fraction of THIS step (0 = off = natural ring / one full step
                    // for MIDI-out). Applies to internal sounds AND MIDI-out notes.
                    // A merged-chain HEAD with value 0 is NOT off - 0 on a chain means "gate the
                    // WHOLE chain" (the native convention; quantised full-length notes land here).
                    // Displaying it as "Off" read as ungated (user bug report), so it shows 100%.
                    const bool chainHead = step + 1 < DrumChannel::MAX_STEPS && merge[ch][step + 1]
                                           && ! merge[ch][step];
                    float ln = juce::jlimit(0.0f, 1.0f, noteLen[ch][step]);
                    if (chainHead && ln <= 0.001f) ln = 1.0f;
                    if (ln > 0.001f)
                    {
                        g.setColour(juce::Colour(0xff8a7adf).withAlpha(alpha));
                        g.fillRect(juce::Rectangle<float>(r.getX(), r.getCentreY() - r.getHeight() * 0.22f,
                                                          ln * r.getWidth(), r.getHeight() * 0.44f).reduced(1.0f, 0.0f));
                        txt = juce::String(juce::roundToInt(ln * 100.0f)) + "%";
                    }
                    else
                    {
                        g.setColour(juce::Colour(0xff8a7adf).withAlpha(alpha * 0.5f));
                        g.drawRect(juce::Rectangle<float>(r.getX(), r.getCentreY() - r.getHeight() * 0.22f,
                                                          r.getWidth(), r.getHeight() * 0.44f).reduced(1.0f, 0.0f), 1.0f);
                        txt = "Off";
                    }
                }
                else
                {
                    const float v01 = juce::jlimit(0.0f, 1.0f, vel[ch][step]);   // ModeVel (1D - Length has its own mode)
                    const float h = v01 * r.getHeight();
                    g.setColour(juce::Colour(0xff22cc55).withAlpha(alpha));
                    g.fillRect(juce::Rectangle<float>(r.getX(), r.getBottom() - h, r.getWidth(), h).reduced(1.0f, 0.0f));
                    txt = juce::String(juce::roundToInt(v01 * 100.0f)) + "%";
                }
                if (editMode == ModePitch)
                {
                    // SLIDE strip (bottom third of the cell = an easy click target): toggles the
                    // glide of THIS step's pitch into the NEXT step's pitch. ON = solid violet band
                    // + white "SLIDE" + arrow at the next step; OFF = a faint band with dim "slide".
                    const float sy = r.getBottom() - r.getHeight() * 0.32f;
                    const auto vio = juce::Colour(0xffb46bff);      // brighter, more saturated violet
                    juce::Rectangle<float> zone(r.getX() + 1.0f, sy, r.getWidth() - 2.0f, r.getBottom() - sy - 1.0f);
                    const bool wide = rr.getWidth() > 20;
                    const float fs = juce::jlimit(8.0f, 12.0f, r.getWidth() * (magnified ? 0.22f : 0.30f));
                    if (slide[ch][step])
                    {
                        g.setColour(vio.withAlpha(alpha * 0.85f)); g.fillRoundedRectangle(zone, 2.5f);   // SOLID band = obvious
                        const float my = zone.getCentreY();
                        // Arrow at the right edge (points at the next step it glides to).
                        g.setColour(juce::Colours::white.withAlpha(alpha));
                        juce::Path tip; tip.addTriangle(zone.getRight() - 2.0f, my,
                                                        zone.getRight() - 10.0f, my - 5.0f,
                                                        zone.getRight() - 10.0f, my + 5.0f);
                        g.fillPath(tip);
                        if (wide) { g.setFont(juce::Font(fs, juce::Font::bold));   // high-contrast label
                                    g.drawText("SLIDE", zone.withTrimmedRight(10.0f), juce::Justification::centred, false); }
                    }
                    else if (wide)
                    {
                        g.setColour(vio.withAlpha(0.22f)); g.fillRoundedRectangle(zone, 2.5f);
                        g.setColour(vio.brighter(0.3f).withAlpha(0.85f)); g.setFont(juce::Font(fs, juce::Font::bold));
                        g.drawText("slide", zone, juce::Justification::centred, false);
                    }
                }
                if (isCurrent) { g.setColour(juce::Colour(0xffffcc33)); g.drawRoundedRectangle(r, 4.0f, 1.5f); }
                if (isMerged)
                {
                    paintMergeArrow(g, ch, step, r);   // thin purple arrow: run start -> run end
                    txt = "";
                }
                if ((rr.getWidth() > 20 || magnified) && txt.isNotEmpty())
                {
                    g.setColour(juce::Colours::white.withAlpha(isActive ? 0.9f : 0.4f));
                    g.setFont(juce::Font(magnified ? 15.0f : 9.5f, juce::Font::bold));
                    g.drawText(txt, rr.getX() + 2, rr.getY() + 3, rr.getWidth() - 4, magnified ? 18 : 12,
                               juce::Justification::centredTop, false);
                }
        }
        paintCellExtras(g, ch, step, rr, r, isActive, isCurrent);
        if (magnified)   // outline the enlarged cell so it clearly floats above its neighbours
        {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawRoundedRectangle(r, 5.0f, 2.0f);
        }
    }
}

// MERGE visual: ONE thin purple ARROW per merged run - from the middle of the run's FIRST
// step to the middle of its LAST step, passing straight through anything in between. Each
// merged cell draws its shaft segment back to the previous cell's centre; the run's last
// cell adds the arrowhead (its tip sits at that cell's centre).
void StepGridComponent::paintMergeArrow(juce::Graphics& g, int ch, int step, juce::Rectangle<float> r)
{
    const bool isLast = step + 1 >= numSteps[ch] || ! merge[ch][step + 1];
    const float cy = r.getCentreY(), cx = r.getCentreX();
    const float th = r.getHeight() * 0.20f;                     // half the old band height
    const float prevCX = (float) stepRect(ch, step - 1).getCentreX();   // step 0 can't be merged
    const float headLen = isLast ? r.getHeight() * 0.32f : 0.0f;
    g.setColour(juce::Colour(0xffb46bff).withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(prevCX, cy - th * 0.5f, cx - headLen - prevCX, th));
    if (isLast)
    {
        juce::Path p;
        p.addTriangle(cx, cy, cx - headLen, cy - r.getHeight() * 0.24f,
                              cx - headLen, cy + r.getHeight() * 0.24f);
        g.fillPath(p);
    }
}

// ===== DRAW MODE canvas =====================================================
juce::Rectangle<int> StepGridComponent::drawRowRect(int ch) const
{ return { 0, (ch - firstRow) * rowH, getWidth(), rowH }; }

// The BIG piano-roll EDITOR panel (user: "much bigger so users can actually see notes"). Nearly the
// whole grid tall, full width, clamped inside the grid. It is a TOGGLE (the lens on the row opens
// it, close/click-outside dismisses).
juce::Rectangle<int> StepGridComponent::drawOverlayRect() const
{
    if (drawMagCh < 0) return {};
    return { 0, 0, getWidth(), getHeight() };   // the editor covers the WHOLE sequencer area (user)
}

int StepGridComponent::drawColAt(int x) const
{ return juce::jlimit(0, totalCols() - 1, (int) ((float) x / (float) juce::jmax(1, getWidth()) * (float) totalCols())); }

int StepGridComponent::yToDrawSemi(juce::Rectangle<int> rect, int y, int range, int centre) const
{
    const float half = juce::jmax(1.0f, rect.getHeight() * 0.5f - 4.0f);
    const float frac = ((float) rect.getCentreY() - (float) y) / half;   // +1 top .. -1 bottom
    return juce::jlimit(-DrumChannel::PITCH_RANGE, DrumChannel::PITCH_RANGE, centre + juce::jlimit(-range, range, (int) std::lround(frac * (float) range)));
}

// Topmost note covering (col, semi) - semi tolerance +-1 so thin bars are grabbable. -1 = none.
int StepGridComponent::prNoteAt(int ch, int col, int semi) const
{
    for (int i = drawNoteCount[ch] - 1; i >= 0; --i)   // last drawn wins (drawn on top)
    {
        const auto& n = drawNotes[ch][i];
        if (col >= n.start && col < n.start + n.len && std::abs((int) n.semi - semi) <= 1) return i;
    }
    return -1;
}

// Remove / trim / split every note that crosses columns [lo..hi] (ANY pitch) - the line stroke and
// the right-drag eraser both clear what they pass over, like the old column lane did.
void StepGridComponent::eraseColRange(int ch, int lo, int hi)
{
    for (int i = drawNoteCount[ch] - 1; i >= 0; --i)
    {
        auto& n = drawNotes[ch][i];
        if (strokeNoteIdx == i) continue;                       // never eat the note we're drawing
        const int a = n.start, b = n.start + n.len - 1;         // inclusive span
        if (b < lo || a > hi) continue;                         // no overlap
        if (a >= lo && b <= hi)                                 // fully covered -> remove
        {
            for (int j = i; j < drawNoteCount[ch] - 1; ++j) drawNotes[ch][j] = drawNotes[ch][j + 1];
            --drawNoteCount[ch];
            if (strokeNoteIdx > i) --strokeNoteIdx;             // keep the stroke handle stable
            continue;
        }
        if (a < lo && b > hi)                                    // covers the range -> split in two
        {
            const int rightStart = hi + 1, rightLen = b - hi;
            n.len = (int16_t) (lo - a);
            if (drawNoteCount[ch] < DrumChannel::DRAW_MAX_NOTES)
                drawNotes[ch][drawNoteCount[ch]++] = { (int16_t) rightStart, (int16_t) rightLen, n.semi, n.vel, n.slot };
            continue;
        }
        if (a < lo)      n.len = (int16_t) (lo - a);            // overlaps from the left -> trim tail
        else           { n.len = (int16_t) (b - hi); n.start = (int16_t) (hi + 1); }   // from the right -> trim head
    }
}

// ROW line gesture: the stroke lays a note that extends as you drag; changing pitch mid-stroke
// starts a new note (a wiggly line = a run of notes). It is POLYPHONIC like the magnified editor -
// drawing OVER existing notes layers on top, it never deletes them (user: the row must not act mono).
// Right-drag = the explicit eraser.
void StepGridComponent::drawStrokeTo(int ch, juce::Point<int> pos)
{
    const auto rect = drawRowRect(ch);
    // PITCH LOCK (user): the stroke stays at the pitch where the press landed - dragging only extends
    // the note in time (the magnified editor already worked this way; the free-pitch row drawing made
    // it too easy to wander off-key). Release + press at another height = the next pitch.
    const int semiRaw = yToDrawSemi(rect, pos.y, 36);
    if (! drawErase && strokeLockSemi <= -100) strokeLockSemi = semiRaw;
    const int semi = drawErase ? semiRaw : strokeLockSemi;
    const int col = drawColAt(pos.x);
    const int lo = juce::jmin(col, drawLastCol < 0 ? col : drawLastCol);
    const int hi = juce::jmax(col, drawLastCol < 0 ? col : drawLastCol);
    if (drawErase)
    {
        strokeNoteIdx = -1;
        eraseColRange(ch, lo, hi);
        drawReadSemi = -128;
    }
    else
    {
        auto& cnt = drawNoteCount[ch];   // POLY: do NOT erase under the stroke - notes layer, never delete
        if (strokeNoteIdx >= 0 && strokeNoteIdx < cnt && (int) drawNotes[ch][strokeNoteIdx].semi == semi)
        {   // extend the current stroke note to cover [lo..hi] - it may cross bar lines (continuous note)
            auto& n = drawNotes[ch][strokeNoteIdx];
            const int a = juce::jmin((int) n.start, lo), b = juce::jmax((int) n.start + n.len - 1, hi);
            n.start = (int16_t) a; n.len = (int16_t) juce::jmax(1, b - a + 1);
        }
        else if (cnt < MIR_MAX)                                 // new pitch (or first stroke) -> new note
        {
            drawNotes[ch][cnt] = { (int16_t) lo, (int16_t) juce::jmax(1, hi - lo + 1), (int8_t) semi,
                                   (uint8_t) juce::jlimit(0, 255, (int) std::lround(dVel[ch] * 255.0f)),
                                   (uint8_t) prTargetSlot };
            strokeNoteIdx = cnt; ++cnt; strokeCreatedNew = true;
        }
        drawReadSemi = semi;   // live read-out
        if (onRollPreview) onRollPreview(semi);   // hear the drawn pitch
    }
    pushNotes(ch);
    drawLastCol = col;
    repaint();
}


// Piano-roll header controls - ONE geometry for paint AND hit-testing (the header is PR_HEAD=32 tall).
static juce::Rectangle<int> prHdrRange(const juce::Rectangle<int>& ov, int i) { return { ov.getX() + 6 + i * 50,      ov.getY() + 4, 46, 24 }; }
static juce::Rectangle<int> prHdrGrid (const juce::Rectangle<int>& ov)        { return { ov.getX() + 6 + 4 * 50 + 10, ov.getY() + 4, 84, 24 }; }
static juce::Rectangle<int> prHdrSlot (const juce::Rectangle<int>& ov, int i)
{ static const int x[3] = { 308, 388, 452 }, w[3] = { 76, 60, 60 }; return { ov.getX() + x[i], ov.getY() + 4, w[i], 24 }; }
static juce::Rectangle<int> prHdrClose(const juce::Rectangle<int>& ov)        { return { ov.getRight() - 30, ov.getY() + 4, 24, 24 }; }
static juce::Rectangle<int> prHdrTune (const juce::Rectangle<int>& ov)        { return { ov.getX() + 522, ov.getY() + 4, 96, 24 }; }
static juce::Rectangle<int> prHdrQuant(const juce::Rectangle<int>& ov)        { return { ov.getX() + 626, ov.getY() + 4, 92, 24 }; }

// Piano-roll note colours by slot target - the SAME family as the keyboard highlight:
// slot 1 = yellow, slot 2 = pink, both = the 50/50 orange blend.
static juce::Colour slotNoteColour(int slot)
{
    return slot == 1 ? juce::Colour(0xffe8bf4d)
         : slot == 2 ? juce::Colour(0xffe86aa8)
                     : juce::Colour(0xffe8bf4d).interpolatedWith(juce::Colour(0xffe86aa8), 0.5f);
}

void StepGridComponent::paintDrawLane(juce::Graphics& g, int ch, juce::Rectangle<int> rect, bool overlay)
{
    const int R = totalCols();   // merged group: the lane spans every bar side by side
    const bool dim = muted[ch] || (anySolo && ! soloed[ch]);
    // (ModeVel/ModePan roll display removed - the roll forces ModeSteps; per-note vel/pan is the right-click menu.)
    const int range = overlay ? drawRange : DrumChannel::PITCH_RANGE;
    const int ctr   = overlay ? juce::jlimit(-prViewClamp(), prViewClamp(), drawViewCenter) : 0;
    juce::Rectangle<int> lane = overlay ? prLane(rect) : rect;
    g.setColour(juce::Colour(overlay ? 0xff1c1c34 : 0xff141426)); g.fillRect(rect);
    const float cy = (float) lane.getCentreY();
    const float half = juce::jmax(1.0f, lane.getHeight() * 0.5f - 4.0f);
    auto yFor = [&](int semi) { return cy - (float) (semi - ctr) / (float) range * half; };
    const float rowH2 = half / (float) range;                 // pixel height of one semitone row
    if (overlay)
    {
        // PIANO-ROLL background: black-key rows shaded + the LEFT note-name column (C3 = semi 0).
        // The column is also a SCROLL handle (drag it up/down when the range is < +-36); with big
        // rows EVERY note is named, with tight rows just the Cs. The hovered/drawn note's row is lit.
        static const bool blackPc[12] = { false,true,false,true,false,false,true,false,true,false,true,false };
        static const char* pcName[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        juce::Rectangle<int> keys(rect.getX(), lane.getY(), PR_KEYS, lane.getHeight());
        g.setColour(juce::Colour(0xff20203a)); g.fillRect(keys);
        const bool nameAll = rowH2 >= 9.0f;                   // enough height to label every semitone
        for (int s = ctr - range; s <= ctr + range; ++s)
        {
            if (s < -DrumChannel::PITCH_RANGE || s > DrumChannel::PITCH_RANGE) continue;
            const float y = yFor(s);
            const int pc = ((s % 12) + 12) % 12;
            if (blackPc[pc])
            {
                g.setColour(juce::Colour(0x16000000));
                g.fillRect(juce::Rectangle<float>((float) lane.getX(), y - rowH2 * 0.5f, (float) lane.getWidth(), rowH2));
                g.setColour(juce::Colour(0x66101018));
                g.fillRect(juce::Rectangle<float>((float) keys.getX(), y - rowH2 * 0.5f, (float) keys.getWidth(), rowH2));
            }
            const bool isHover = (drawReadSemi != -128 ? s == drawReadSemi   // drawing/hovering a note
                                                       : s == prHoverSemi);  // else: the row under the cursor
            if (isHover)   // highlight the active key row
            {
                g.setColour(juce::Colour(0x8835c0ff));
                g.fillRect(juce::Rectangle<float>((float) keys.getX(), y - rowH2 * 0.5f, (float) keys.getWidth(), juce::jmax(3.0f, rowH2)));
            }
            const int oct = 4 + (int) std::floor((double) s / 12.0);   // semi 0 = middle C = C4 (scientific)
            if (nameAll || pc == 0 || isHover)
            {
                g.setColour(isHover ? juce::Colours::white : juce::Colour(pc == 0 ? 0xffb8c4dc : 0xff8090b0));
                g.setFont(juce::Font(juce::jmin(10.0f, rowH2 + 3.0f), pc == 0 || isHover ? juce::Font::bold : juce::Font::plain));
                g.drawText(juce::String(pcName[pc]) + juce::String(oct), keys.getX() + 2, (int) (y - 6.0f), PR_KEYS - 4, 12,
                           juce::Justification::centredLeft, false);
            }
            if (pc == 0)
            {
                g.setColour(juce::Colour(0x2fffffff));
                g.drawHorizontalLine((int) (y + rowH2 * 0.5f), (float) lane.getX(), (float) lane.getRight());
            }
        }
        // SNAP-GRID verticals (the header's Grid selector; 0 = free = none) - the grid repeats PER
        // BAR; bar boundaries draw as strong amber lines in a merged group.
        if (drawGridDiv > 0)
        {
            const int nLines = drawGridDiv * grpBars;
            for (int i = 0; i <= nLines; ++i)
            {
                const float x = (float) lane.getX() + (float) i / (float) nLines * (float) lane.getWidth();
                g.setColour(juce::Colour(i % 4 == 0 ? 0x30ffffff : 0x14ffffff));
                g.fillRect(juce::Rectangle<float>(x - 0.5f, (float) lane.getY(), 1.0f, (float) lane.getHeight()));
            }
        }
        for (int b = 1; b < grpBars; ++b)   // BAR separators (independent of the snap grid)
        {
            const float x = (float) lane.getX() + (float) b / (float) grpBars * (float) lane.getWidth();
            g.setColour(juce::Colour(0xaad9c46a));
            g.fillRect(juce::Rectangle<float>(x - 1.0f, (float) lane.getY(), 2.0f, (float) lane.getHeight()));
        }
    }
    else
    {
        g.setColour(juce::Colour(0x18ffffff));
        for (int s = -range; s <= range; s += 12) if (s != 0) g.drawHorizontalLine((int) yFor(s), (float) lane.getX(), (float) lane.getRight());
    }
    if (ctr - range <= 0 && 0 <= ctr + range)   // the C3 (pitch 0) reference line, only while in view
    { g.setColour(juce::Colour(0x66ffe9b0)); g.fillRect(juce::Rectangle<float>((float) lane.getX(), yFor(0) - 0.5f, (float) lane.getWidth(), 1.0f)); }
    // NOTE BARS - CLIPPED to the lane so nothing spills out of the box; notes outside the visible
    // pitch window draw as thin edge indicators (kept, just off-view).
    const float colW = (float) lane.getWidth() / (float) R;
    g.saveState(); g.reduceClipRegion(lane);
    if (overlay && getSlotVoicing)
    {   // GHOST LINES: the pitches each slot ACTUALLY sounds for every note (chord/scale voicing +
        // slot-2 transpose) - faint, non-editable, slot-coloured like the keyboard highlight.
        for (int i = 0; i < drawNoteCount[ch]; ++i)
        {
            const auto& n = drawNotes[ch][i];
            const float gx1 = (float) lane.getX() + (float) n.start * colW;
            const float gx2 = (float) lane.getX() + (float) (n.start + n.len) * colW;
            const float gh = juce::jmax(2.0f, rowH2 * 0.55f);
            for (int sIdx = 0; sIdx < DrumChannel::NUM_SLOTS; ++sIdx)
            {
                if (n.slot != 0 && (int) n.slot != sIdx + 1) continue;   // the note doesn't play this slot
                int vo[8]; const int cnt = getSlotVoicing(ch, sIdx, (int) n.semi, vo);
                for (int k = 0; k < cnt; ++k)
                {
                    if (vo[k] == (int) n.semi) continue;                 // the solid bar already shows it
                    if (vo[k] < ctr - range || vo[k] > ctr + range) continue;
                    g.setColour((sIdx == 0 ? juce::Colour(0xffe8bf4d) : juce::Colour(0xffe86aa8)).withAlpha(0.30f));
                    g.fillRect(juce::Rectangle<float>(gx1, yFor(vo[k]) - gh * 0.5f, juce::jmax(2.0f, gx2 - gx1 - 1.0f), gh));
                }
            }
        }
    }
    for (int i = 0; i < drawNoteCount[ch]; ++i)
    {
        const auto& n = drawNotes[ch][i];
        const float x1 = (float) lane.getX() + (float) n.start * colW;
        const float x2 = (float) lane.getX() + (float) (n.start + n.len) * colW;
        const bool outOfView = (int) n.semi < ctr - range || (int) n.semi > ctr + range;
        const float y = yFor(juce::jlimit(ctr - range, ctr + range, (int) n.semi));
        const float bh = outOfView ? 2.0f : (overlay ? juce::jmax(3.0f, rowH2 - 1.0f) : 4.0f);
        const auto slotCol = slotNoteColour((int) n.slot);
        auto col = dim ? juce::Colour(0xff5f6a78) : outOfView ? slotCol.withAlpha(0.5f) : slotCol;
        if (overlay && ! outOfView)
        {
            juce::Rectangle<float> bar(x1, y - bh * 0.5f, juce::jmax(2.0f, x2 - x1 - 1.0f), bh);
            if (n.oneShot)
            {   // ONE-SHOT: the body ends in a fading TAPER (the sound rings past the bar; no gate,
                // so no square end/resize emphasis). Right-click menu toggles one-shot/gated.
                const float tw = juce::jmin(10.0f, bar.getWidth() * 0.45f);
                auto body = bar.withTrimmedRight(tw);
                g.setColour(col.withAlpha(0.9f)); g.fillRoundedRectangle(body, 2.0f);
                juce::Path taper;
                taper.addTriangle(body.getRight(), bar.getY(), body.getRight(), bar.getBottom(),
                                  bar.getRight(), bar.getCentreY());
                g.setColour(col.withAlpha(0.45f)); g.fillPath(taper);
                g.setColour(juce::Colour(0xff101018)); g.drawRoundedRectangle(body, 2.0f, 1.0f);
            }
            else
            {
                g.setColour(col.withAlpha(0.9f)); g.fillRoundedRectangle(bar, 2.0f);
                g.setColour(juce::Colour(0xff101018)); g.drawRoundedRectangle(bar, 2.0f, 1.0f);
            }
            if (prSel[i])   // part of the multi-selection -> amber outline
            { g.setColour(juce::Colour(0xffffc23a)); g.drawRoundedRectangle(bar.expanded(1.0f), 3.0f, 1.6f); }
            if (! n.oneShot && bar.getWidth() > 14.0f)   // resize tab on the right edge (gated notes)
            { g.setColour(juce::Colours::white.withAlpha(0.55f));
              g.fillRect(juce::Rectangle<float>(bar.getRight() - 4.0f, bar.getY() + 1.0f, 3.0f, bar.getHeight() - 2.0f)); }
            if (n.glide)   // GLIDE flag: a cyan ramp sliding UP into the note's left edge (portamento)
            { g.setColour(juce::Colour(0xff35c0ff));
              g.drawLine(x1 - 8.0f, bar.getBottom() + 3.0f, x1, bar.getY(), 2.2f); }
        }
        else
        {
            g.setColour(col);
            g.fillRect(juce::Rectangle<float>(x1, y - bh * 0.5f, juce::jmax(2.0f, x2 - x1 - 1.0f), bh));
            if (n.glide)   // tiny cyan tick at the note start (glide) in the compact row view
            { g.setColour(juce::Colour(0xff35c0ff)); g.fillRect(juce::Rectangle<float>(x1 - 2.0f, y - bh, 2.0f, bh * 2.0f)); }
        }
    }
    if (overlay && prMode == 5)   // MARQUEE rubber band (right-drag)
    {
        const auto mr = juce::Rectangle<int>::leftTopRightBottom(juce::jmin(prMarqA.x, prMarqB.x), juce::jmin(prMarqA.y, prMarqB.y),
                                                                 juce::jmax(prMarqA.x, prMarqB.x), juce::jmax(prMarqA.y, prMarqB.y));
        g.setColour(juce::Colour(0x30ffc23a)); g.fillRect(mr);
        g.setColour(juce::Colour(0xccffc23a)); g.drawRect(mr, 1);
    }
    g.restoreState();
    if (playBarFrac > 0.0f) { const float px = (float) lane.getX() + playBarFrac * (float) lane.getWidth();
        g.setColour(juce::Colour(0x88ffcc33)); g.fillRect(juce::Rectangle<float>(px - 0.5f, (float) lane.getY(), 1.0f, (float) lane.getHeight())); }

    if (! overlay)
    {
        // the LENS button (top-left) that opens the piano-roll editor - a drawn magnifier glyph.
        juce::Rectangle<float> lb((float) rect.getX() + 2.0f, (float) rect.getY() + 2.0f, 18.0f, 15.0f);
        g.setColour(juce::Colour(0x33ffffff)); g.fillRoundedRectangle(lb, 3.0f);
        g.setColour(juce::Colour(0xcce8e8f4));
        g.drawEllipse(lb.getX() + 3.0f, lb.getY() + 3.0f, 7.0f, 7.0f, 1.4f);            // lens
        g.drawLine(lb.getX() + 9.5f, lb.getY() + 9.5f, lb.getX() + 13.5f, lb.getY() + 12.5f, 1.6f);  // handle
    }
    else
    {
        // header: range radios | snap-grid selector | title | close (right)
        g.setColour(juce::Colour(0xff262646)); g.fillRect(rect.getX(), rect.getY(), rect.getWidth(), PR_HEAD);
        static const int ranges[4] = { 6, 12, 24, 48 };
        for (int i = 0; i < 4; ++i) {
            const auto rr = prHdrRange(rect, i);
            const bool on = (drawRange == ranges[i]);
            g.setColour(on ? juce::Colour(0xff35c0ff) : juce::Colour(0xff33335a)); g.fillRoundedRectangle(rr.toFloat(), 4.0f);
            g.setColour(on ? juce::Colours::black : juce::Colour(0xffb8b8d0)); g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText(juce::String::fromUTF8("\xc2\xb1") + juce::String(ranges[i]), rr, juce::Justification::centred, false);
        }
        { // snap grid: CLICK to type any 1/N (1-64, 0 = off)
            const auto gr = prHdrGrid(rect);
            g.setColour(juce::Colour(0xff33335a)); g.fillRoundedRectangle(gr.toFloat(), 4.0f);
            g.setColour(juce::Colour(0xffd9c46a)); g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText(drawGridDiv > 0 ? "Grid 1/" + juce::String(drawGridDiv) : "Grid off", gr, juce::Justification::centred, false);
        }
        { // DRAW TARGET buttons: which slot(s) a new note plays (orange = both, yellow = slot 1, pink =
          // slot 2, like the keyboard highlight). Clicking one with a live selection RE-TAGS those notes.
            static const char* nm[3] = { "Both slots", "Slot 1", "Slot 2" };
            for (int i = 0; i < 3; ++i)
            {
                const auto sr = prHdrSlot(rect, i);
                const auto sc = slotNoteColour(i);
                if (prTargetSlot == i) { g.setColour(sc); g.fillRoundedRectangle(sr.toFloat(), 4.0f); g.setColour(juce::Colours::black); }
                else                   { g.setColour(juce::Colour(0xff33335a)); g.fillRoundedRectangle(sr.toFloat(), 4.0f); g.setColour(sc); }
                g.setFont(juce::Font(12.5f, juce::Font::bold));
                g.drawText(nm[i], sr, juce::Justification::centred, false);
            }
        }
        { // TUNE drag-fader (Arp-Rate style): shifts the WHOLE bar +-50 cents. The roll's 0-point
          // becomes C4 + this; leaving the roll parks the Base Freq knob at the tuned value.
            const auto tr = prHdrTune(rect);
            const float tv = juce::jlimit(-50.0f, 50.0f, drawTune[ch]);
            g.setColour(juce::Colour(0xff33335a)); g.fillRoundedRectangle(tr.toFloat(), 4.0f);
            g.setColour(juce::Colour(0x5535c0ff));   // faint fill showing the position (centre = 0)
            const float fx = (float) tr.getX() + (tv + 50.0f) / 100.0f * (float) tr.getWidth();
            const float cx0 = (float) tr.getCentreX();
            g.fillRect(juce::Rectangle<float>(juce::jmin(cx0, fx), (float) tr.getY() + 2.0f,
                                              juce::jmax(2.0f, std::abs(fx - cx0)), (float) tr.getHeight() - 4.0f));
            g.setColour(juce::Colour(0xff9fdcff)); g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText("Tune " + juce::String(tv > 0 ? "+" : "") + juce::String((int) std::lround(tv)) + "c",
                       tr, juce::Justification::centred, false);
        }
        { // QUANTIZE: CLICK to type 1/N - snaps every note's START to that grid ONCE (not the live snap grid).
            const auto qr = prHdrQuant(rect);
            g.setColour(juce::Colour(0xff2a3a4a)); g.fillRoundedRectangle(qr.toFloat(), 4.0f);
            g.setColour(juce::Colour(0xff8fd0a0)); g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText("Quantize", qr, juce::Justification::centred, false);
        }
        g.setColour(juce::Colour(0xff9aa4c0)); g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("drag=draw - dbl-click=delete - RIGHT-CLICK=note menu - RIGHT-DRAG=select area",
                   rect.getX() + 724, rect.getY(), rect.getWidth() - 724 - 34, PR_HEAD, juce::Justification::centredLeft, false);
        const auto cl = prHdrClose(rect);
        g.setColour(juce::Colour(0xff5a2a2a)); g.fillRoundedRectangle(cl.toFloat(), 4.0f);
        g.setColour(juce::Colours::white); g.setFont(juce::Font(15.0f, juce::Font::bold));
        g.drawText("x", cl, juce::Justification::centred, false);
    }
    // read-out (semitones from 0): a big low-alpha WATERMARK in the top-right.
    if (drawReadSemi != -128)
    {
        const juce::String t = (drawReadSemi > 0 ? "+" : "") + juce::String(drawReadSemi);
        const float fs = overlay ? 34.0f : juce::jmin(30.0f, (float) rect.getHeight() * 0.7f);
        g.setColour(juce::Colours::white.withAlpha(0.16f)); g.setFont(juce::Font(fs, juce::Font::bold));
        g.drawText(t + " st", rect.getRight() - 150, lane.getY(), 146, lane.getHeight(), juce::Justification::centredRight, false);
    }
    if (overlay) { g.setColour(juce::Colours::white.withAlpha(0.85f)); g.drawRect(rect, 2); }
    if (ch == selectedRow && ! overlay) { g.setColour(juce::Colour(0xffff3b30)); g.drawRect(rect.reduced(1), 2); }
}

void StepGridComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff161626));

    const int last = juce::jmin(firstRow + visibleRows, Sequencer::NUM_CHANNELS);
    for (int ch = juce::jmax(0, firstRow); ch < last; ++ch)
    {
        if (drawMode[ch]) { paintDrawLane(g, ch, drawRowRect(ch), false); continue; }   // DRAW lane replaces the step cells
        bool effectiveMute = muted[ch] || (anySolo && !soloed[ch]);
        int n = numSteps[ch];

        // SELECTED channel: a bright RED outline around the whole row (washes/bars were either
        // invisible or muddy on the dark UI - user picked the outline).
        if (ch == selectedRow && n > 0)
        {
            auto r0 = stepRect(ch, 0), r1 = stepRect(ch, n - 1);
            g.setColour(juce::Colour(0xffff3b30));
            g.drawRoundedRectangle((float) r0.getX() - 3.0f, (float) r0.getY() + 0.5f,
                                   (float)(r1.getRight() - r0.getX()) + 6.0f, (float) r0.getHeight() - 1.0f,
                                   5.0f, 1.6f);
        }

        // MERGED GROUP: amber separators where each bar begins (the row = the bars side by side).
        if (grpBars > 1 && n > 0)
            for (int b = 1; b < grpBars; ++b)
            {
                const int s0 = barStep0[ch][b];
                if (s0 <= 0 || s0 >= n) continue;
                const auto rr = stepRect(ch, s0);
                g.setColour(juce::Colour(0xaad9c46a));
                g.fillRect(juce::Rectangle<float>((float) rr.getX() - 1.5f, (float) rr.getY() + 1.0f, 2.5f, (float) rr.getHeight() - 2.0f));
            }

        for (int step = 0; step < n; ++step)
        {
            if (editMode != ModeSteps)
            {
                if (ch == magCh && step == magStep) continue;   // drawn by the overlay at 1.5x (magnifier)
                paintValueCell(g, ch, step, stepRect(ch, step), false);
                continue;
            }

            auto rr = stepRect(ch, step);
            auto r  = rr.toFloat().reduced(3.5f, 2.0f);   // taller cells (small horiz gap; vertical drag-lock prevents mis-hits)
            bool isActive  = steps[ch][step];
            bool isCurrent = (playStep[ch] == step);

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

            if (merge[ch][step])   // MERGE: the previous step's note holds THROUGH this one
                paintMergeArrow(g, ch, step, r);

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

            paintCellExtras(g, ch, step, rr, r, isActive, isCurrent);
        }
    }
    // The magnified cell is drawn by StepMagnifierOverlay (a top-most sibling) so it can float
    // above the top bar / channel strips, which would otherwise clip a first-row / -column cell.

    // DRAW mode 4x-vertical magnify: while a stroke is in progress, redraw that row's lane enlarged
    // ON TOP so the pitch is placeable (the base row is only ~44 px for +/-36 semitones).
    if (drawMagCh >= 0)
        paintDrawLane(g, drawMagCh, drawOverlayRect(), true);
}

juce::String StepGridComponent::getTooltip()
{
    const int dch = firstRow + juce::jmax(0, getMouseXYRelative().y) / juce::jmax(1, rowH);
    if (dch >= 0 && dch < NCH && drawMode[dch])
        return juce::String("PIANO ROLL (free notes; pitch 0 = C4 (middle C), always).\n\n"
                            "- LEFT-drag draws/moves notes; RIGHT-drag erases; the magnifier (top-left) opens the "
                            "BIG editor.\n"
                            "- The colour buttons pick which SOUND SLOT new notes play: orange = both, yellow = "
                            "slot 1, pink = slot 2 (same as the keyboard). SHIFT+select notes, then click a colour "
                            "to move them.\n"
                            "- Faint lines show every pitch each slot REALLY sounds (chords, scales, slot-2 pitch) "
                            "- display only.\n"
                            "- CMD/CTRL+click a note = GLIDE (slides in from the previous note; the KEYS Glide "
                            "knob sets the time).\n"
                            "- RIGHT-CLICK a note (no drag) = the NOTE MENU: Gate on/off, glide, slot, delete. "
                            "GATE OFF (tapered end) = fires like a step and rings naturally; GATE ON (square end) "
                            "= holds at sustain for its length, then releases.\n"
                            "- RIGHT-DRAG = SELECT an area (marquee): then drag any selected note to move them all "
                            "together (the note menu edits all of them), double-click deletes them, a colour button "
                            "re-slots them.\n"
                            "- TUNE (header fader): shifts the WHOLE bar up to +-50 cents (tape-style detune). The "
                            "keyboard follows it on this channel, and switching to steps keeps the transposed "
                            "0-point on the Base Freq knob.\n\n"
                            "Humanize/Strum apply here too. Pick a step count in the dropdown to QUANTISE the roll "
                            "to steps.");
    return juce::String("STEP GRID. Click = toggle steps; the buttons top-right switch edit modes (Vel/Len/Pitch/"
                        "Loop/Roll/Pan). Hold a step in any value mode to MAGNIFY it for fine edits.\n\n"
                        "MERGE: Shift + CLICK a step to merge it into the previous step's note - the run plays as "
                        "ONE long note (shown as a purple ARROW through the run; great for held bass notes / keys "
                        "recordings). Click the same way again to unmerge. A merged step uses its run's first step's "
                        "values - editing it edits that first step.");
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
        else if (editMode == ModeLen)   noteLen[ch][s] = noteLen[ch][srcStep];
        else if (editMode == ModePitch) { pit[ch][s] = pit[ch][srcStep]; slide[ch][s] = slide[ch][srcStep]; }
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
    const int ch = dragChannel, step = dragStep;   // dragStep is already the chain head (see mouseDown)
    auto r = activeStepRect(ch, step);             // the magnified rect while held = finer value travel
    const float v01 = juce::jlimit(0.0f, 1.0f, 1.0f - (float)(pos.y - r.getY()) / (float) juce::jmax(1, r.getHeight()));
    float value = v01;                                   // Velocity / Probability
    if (editMode == ModePitch)     value = (v01 * 2.0f - 1.0f) * (float) DrumChannel::PITCH_RANGE;   // +-48 semis (C-1..C7, matches the keys)
    else if (editMode == ModeRoll) value = (float)(1 + juce::roundToInt(v01 * 5.0f)); // 1..6
    else if (editMode == ModeNudge) { const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
        value = juce::jlimit(-1.0f, 1.0f, xn * 2.0f - 1.0f);
        if (std::abs(value) < 0.07f) value = 0.0f;   // snap to on-the-grid near the centre
        nudge[ch][step] = value;
    }
    else if (editMode == ModePan)  { const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
                                     value = juce::jlimit(-1.0f, 1.0f, (xn - 0.5f) * 2.0f); }   // X = pan -1..+1

    if (editMode == ModeLen) {                           // X = gate length (0 = off/natural; snaps to Off near 0)
        const float xn = (float)(pos.x - r.getX()) / (float) juce::jmax(1, r.getWidth());
        value = juce::jlimit(0.0f, 1.0f, xn);
        if (value < 0.04f) value = 0.0f;
        noteLen[ch][step] = value;
    }
    else if (editMode == ModeVel)
        vel[ch][step] = value;                           // Y = velocity
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
    notifyMag();                    // repaint grid + the magnifier overlay (value changed while held)
}

void StepGridComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (drawMagCh >= 0)   // the roll's TUNE fader: double-click = back to 0 (like every knob)
    {
        const auto ov = drawOverlayRect();
        if (prHdrTune(ov).contains(e.getPosition()))
        {
            drawTune[drawMagCh] = 0.0f;
            if (onDrawTuneChanged) onDrawTuneChanged(drawMagCh, 0.0f);
            repaint(); return;
        }
    }
    const auto p = e.getPosition();
    // PIANO ROLL: double-click a note = DELETE it (overlay or row).
    if (drawMagCh >= 0)
    {
        const auto lane = prLane(drawOverlayRect());
        if (lane.contains(p))
        {
            const int ch2 = drawMagCh;
            const int idx = prNoteAt(ch2, prColAt(lane, p.x), yToDrawSemi(lane, p.y, drawRange, drawViewCenter));
            if (idx >= 0 && prSel[idx])   // double-click a SELECTED note = delete the whole selection
            {
                for (int i = drawNoteCount[ch2] - 1; i >= 0; --i)
                    if (prSel[i]) { for (int j = i; j < drawNoteCount[ch2] - 1; ++j) drawNotes[ch2][j] = drawNotes[ch2][j + 1];
                                    --drawNoteCount[ch2]; }
                prClearSel(); pushNotes(ch2); repaint();
            }
            else if (idx >= 0) { prClearSel();
                            for (int j = idx; j < drawNoteCount[ch2] - 1; ++j) drawNotes[ch2][j] = drawNotes[ch2][j + 1];
                            --drawNoteCount[ch2]; pushNotes(ch2); repaint(); }
        }
        return;
    }
    { const int dch = firstRow + (p.y >= 0 ? p.y / juce::jmax(1, rowH) : -1);
      if (dch >= 0 && dch < NCH && drawMode[dch])
      {
          // DOUBLE-CLICK on the small row = MAGNIFY (open the big editor) - user redesign; deleting
          // moved fully into the big editor. The double-click's own FIRST press drew a note - take
          // exactly that one back (never a pre-existing note the user aimed at).
          if (tapNoteCh == dch && tapNoteIdx >= 0 && tapNoteIdx < drawNoteCount[dch])
          {
              for (int j = tapNoteIdx; j < drawNoteCount[dch] - 1; ++j) drawNotes[dch][j] = drawNotes[dch][j + 1];
              --drawNoteCount[dch]; pushNotes(dch);
          }
          tapNoteCh = -1; tapNoteIdx = -1;
          drawMagCh = dch; repaint();
          return;
      } }
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
        case ModeVel:   vel[ch][step] = 1.0f; prim = 1.0f; break;
        case ModeLen:   noteLen[ch][step] = 0.0f; prim = 0.0f; break;
        case ModePitch: pit[ch][step] = 0.0f; prim = 0.0f;
                        if (slide[ch][step]) { slide[ch][step] = false;
                                               if (onStepSlideChanged) onStepSlideChanged(ch, step, false); }
                        break;
        case ModePan:   pan[ch][step] = 0.0f; prim = 0.0f; break;
        case ModeNudge: nudge[ch][step] = 0.0f; prim = 0.0f; break;
        case ModeRoll:  roll[ch][step] = 1; rollDec[ch][step] = 0.0f; prim = 1.0f; break;
        default: return;
    }
    if (onStepValueChanged) onStepValueChanged(ch, step, editMode, prim);
    if (influenceArmed[ch]) applyInfluence(ch, step);
    repaint();
}

void StepGridComponent::mouseMove(const juce::MouseEvent& e)
{
    // PIANO-ROLL hover read-out: the note under the cursor - its semitone (Pitch) or velocity% (Vel).
    const auto p = e.getPosition();
    const int prevSemi = drawReadSemi, prevHover = prHoverSemi;
    drawReadSemi = -128; prHoverSemi = -999;
    int hch = -1, col = 0, idx = -1;
    if (drawMagCh >= 0 && prLane(drawOverlayRect()).contains(p))
    {
        hch = drawMagCh;
        const auto lane = prLane(drawOverlayRect());
        col = prColAt(lane, p.x);
        prHoverSemi = yToDrawSemi(lane, p.y, drawRange, drawViewCenter);   // the row under the cursor (always lit)
        idx = prNoteAt(hch, col, prHoverSemi);                             // pitch-proximity hit in the editor
    }
    else
    {
        const int dch = firstRow + (p.y >= 0 ? p.y / juce::jmax(1, rowH) : -1);
        if (dch >= 0 && dch < NCH && drawMode[dch])
        {
            hch = dch; col = drawColAt(p.x);
            for (int i = drawNoteCount[hch] - 1; i >= 0; --i)          // row: topmost note at the column
                if (col >= drawNotes[hch][i].start && col < drawNotes[hch][i].start + drawNotes[hch][i].len) { idx = i; break; }
        }
    }
    if (hch >= 0 && idx >= 0)
    { drawReadSemi = drawNotes[hch][idx].semi; }
    if (drawReadSemi != prevSemi || prHoverSemi != prevHover) repaint();
}

void StepGridComponent::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    // PIANO ROLL: the ROW keeps the quick line-draw gesture (left-drag draws, right-drag erases);
    // the LENS opens the BIG piano-roll EDITOR (pointer gestures: drag empty = create, drag note =
    // move, drag its right edge = resize, double-click = delete; snap grid + range in the header).
    if (drawMagCh >= 0)
    {
        const auto ov = drawOverlayRect();
        if (ov.contains(p))
        {
            const int hy = ov.getY();
            if (prHdrClose(ov).contains(p)) { drawMagCh = -1; drawReadSemi = -128; prMode = 0; repaint(); return; }  // close
            static const int ranges[4] = { 6, 12, 24, 48 };
            for (int i = 0; i < 4; ++i)
                if (prHdrRange(ov, i).contains(p))
                { drawRange = ranges[i];
                  drawViewCenter = juce::jlimit(-prViewClamp(), prViewClamp(), drawViewCenter);   // window stays inside +-36
                  repaint(); return; }   // range radio
            if (prHdrGrid(ov).contains(p))   // snap grid: type a value
            { if (onGridDivEdit) onGridDivEdit(); return; }
            if (prHdrQuant(ov).contains(p))  // QUANTIZE: type 1/N, snap note starts once
            { if (onQuantizeEdit) onQuantizeEdit(); return; }
            if (prHdrTune(ov).contains(p))   // TUNE fader: absolute drag across the box (Arp-Rate style)
            {
                prMode = 7;
                const auto tr = prHdrTune(ov);
                const float tv = juce::jlimit(-50.0f, 50.0f,
                    ((float)(p.x - tr.getX()) / (float) juce::jmax(1, tr.getWidth())) * 100.0f - 50.0f);
                drawTune[drawMagCh] = std::round(tv);
                if (onDrawTuneChanged) onDrawTuneChanged(drawMagCh, drawTune[drawMagCh]);
                repaint(); return;
            }
            {   // DRAW-TARGET buttons: pick which slot new notes play; a live selection gets RE-TAGGED
                for (int i = 0; i < 3; ++i)
                    if (prHdrSlot(ov, i).contains(p))
                    {
                        prTargetSlot = i;
                        if (prSelCount > 0)
                        {
                            for (int j = 0; j < drawNoteCount[drawMagCh]; ++j)
                                if (prSel[j]) drawNotes[drawMagCh][j].slot = (uint8_t) i;
                            pushNotes(drawMagCh);
                        }
                        repaint(); return;
                    }
            }
            const auto lane = prLane(ov);
            if (! lane.contains(p))
            {   // the LEFT note-name column = a SCROLL handle: drag it to shift the pitch window
                if (p.y > hy + PR_HEAD && p.x < lane.getX() && prViewClamp() > 0)
                { prMode = 4; prScrollGrabY = p.y; prScrollGrabC = drawViewCenter; }
                return;
            }
            const int ch2 = drawMagCh;
            const int col = prColAt(lane, p.x);
            const int semi = yToDrawSemi(lane, p.y, drawRange, drawViewCenter);
            const int idx = prNoteAt(ch2, col, semi);
            if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
            {   // RIGHT-CLICK: decided on release. NO drag = the NOTE MENU (on a note); a DRAG =
                // marquee area-select (any pitch). prMode 8 = 'pending right-click' until mouseUp/mouseDrag.
                prMode = 8; prMarqA = prMarqB = p; prRightIdx = idx; return;
            }
            if ((e.mods.isCommandDown() || e.mods.isCtrlDown()) && idx >= 0)
            {   // cmd/ctrl+click toggles GLIDE on this note: it slides in from the previous (legato) note's
                // pitch (Glide knob = time). The piano-roll equivalent of a step's Slide flag.
                drawNotes[ch2][idx].glide = drawNotes[ch2][idx].glide ? 0 : 1;
                pushNotes(ch2); repaint(); return;
            }
            if (idx >= 0 && prSel[idx])
            {   // drag any SELECTED note = move the WHOLE selection together
                for (int i = 0; i < drawNoteCount[ch2]; ++i)
                { prOrigStart[i] = drawNotes[ch2][i].start; prOrigSemi[i] = drawNotes[ch2][i].semi; }
                prIdx = idx; prMode = 6; prGrabDCol = col; prGrabDSemi = semi;
                drawReadSemi = drawNotes[ch2][idx].semi;
            }
            else if (idx >= 0)
            {
                prClearSel();
                const auto& n = drawNotes[ch2][idx];
                const float colW = (float) lane.getWidth() / (float) DrumChannel::DRAW_RES;
                const float noteR = (float) lane.getX() + (float) (n.start + n.len) * colW;
                prIdx = idx;
                prMode = ((float) p.x > noteR - 6.0f) ? 2 : 1;   // near the right edge = RESIZE, else MOVE
                prGrabDCol = col - n.start; prGrabDSemi = semi - n.semi;
                drawReadSemi = n.semi;
            }
            else if (drawNoteCount[ch2] < MIR_MAX)
            {   // empty space -> CREATE (snapped start, one grid cell long; drag right to lengthen).
                // A note lives inside ONE bar (crossing the bar line would re-trigger anyway).
                prClearSel();
                const int start = prSnap(col);
                const int cw = drawGridDiv > 0 ? DrumChannel::DRAW_RES / drawGridDiv : 12;
                const int barEnd = (start / DrumChannel::DRAW_RES + 1) * DrumChannel::DRAW_RES;
                drawNotes[ch2][drawNoteCount[ch2]] = { (int16_t) start, (int16_t) juce::jmax(1, juce::jmin(cw, barEnd - start)),
                                                       (int8_t) juce::jlimit(-DrumChannel::PITCH_RANGE, DrumChannel::PITCH_RANGE, semi),
                                                       (uint8_t) juce::jlimit(0, 255, (int) std::lround(dVel[ch2] * 255.0f)),
                                                       (uint8_t) prTargetSlot };
                prIdx = drawNoteCount[ch2]++; prMode = 3; prGrabDCol = 0; prGrabDSemi = 0;
                drawReadSemi = semi;
                if (onRollPreview) onRollPreview(semi);   // hear what you just drew
                pushNotes(ch2);
            }
            repaint();
            return;
        }
        drawMagCh = -1; drawReadSemi = -128; prMode = 0; repaint();   // click outside closes
        return;
    }
    {
        const int dch = firstRow + (p.y >= 0 ? p.y / juce::jmax(1, rowH) : -1);
        if (dch >= 0 && dch < NCH && drawMode[dch])
        {
            if (onChannelSelected) onChannelSelected(dch);
            const auto row = drawRowRect(dch);
            const bool onLens = juce::Rectangle<int>(row.getX() + 2, row.getY() + 2, 18, 16).contains(p);
            // In the roll the edit mode is always ModeSteps (all step edit-modes are disabled here),
            // so the ROW is pure note drawing: left-drag draws (poly), right-drag erases; lens opens
            // the big editor. Per-note velocity/pan live in the right-click menu.
            if (onLens) { drawMagCh = dch; repaint(); return; }
            if (e.getNumberOfClicks() >= 2) return;   // double-click = magnify (handled in mouseDoubleClick);
                                                      // the 2nd press must not draw another stroke
            drawDragCh = dch; drawErase = e.mods.isRightButtonDown(); drawLastCol = -1; strokeNoteIdx = -1;
            strokeLockSemi = -128; strokeCreatedNew = false;
            drawStrokeTo(dch, p);
            return;
        }
    }
    int ch, step;
    bool onStep = findStepAt(e.getPosition(), ch, step);

    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu())
    {
        if (onStep && midiLearn != nullptr)
        {
            int forced = getLearnChannel ? getLearnChannel() : -1;
            // Alt target: the same step NUMBER on the SELECTED channel (follows selection).
            showMidiLearnMenu(this, *midiLearn, stepParamId(ch, step), forced,
                              "ui_selstep_" + juce::String(step),
                              "SELECTED-channel step " + juce::String(step + 1));
        }
        return;
    }

    if (onStep && onChannelSelected) onChannelSelected(ch);
    // MERGE toggle: SHIFT + click a step (any edit mode) = this step CONTINUES the previous
    // step's note (piano-roll style long note). Step 1 can't continue anything. (Cmd/Ctrl were
    // dropped at user request - shift only.)
    if (onStep && e.mods.isShiftDown())
    {
        if (step > 0)
        {
            merge[ch][step] = ! merge[ch][step];
            if (onStepMergeChanged) onStepMergeChanged(ch, step, merge[ch][step]);
            repaint();
        }
        return;
    }
    if (editMode == ModeSteps) handleClick(e.getPosition(), true);
    else if (editMode == ModeProb)
    {
        // Loop-condition editor: remember where we pressed (drag = set cycle length; click a bar = toggle it).
        condDragCh = onStep ? ch : -1; condDragStep = step; condDragged = false; condDownX = e.getPosition().x;
        if (onStep) beginMagnify(ch, step, e.getPosition());   // 1.5x cell, anchored at the cursor
        if (onStep) { auto r = activeStepRect(ch, step); const int N = juce::jmax(1, condLen[ch][step]);
            const float xn = juce::jlimit(0.0f, 0.999f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
            condDownBar = juce::jlimit(0, N - 1, (int)(xn * (float) N)); }
    }
    else
    {
        // Pitch mode: the bottom THIRD of a cell is the SLIDE strip - a click there toggles the
        // glide of this step's pitch into the NEXT step's pitch instead of starting a pitch drag.
        if (editMode == ModePitch && onStep)
        {
            auto r = stepRect(ch, step);
            if (e.getPosition().y > r.getBottom() - juce::roundToInt(r.getHeight() * 0.32f))
            {
                slide[ch][step] = ! slide[ch][step];
                if (onStepSlideChanged) onStepSlideChanged(ch, step, slide[ch][step]);
                // Influence armed: copy ONLY the slide flag onto every step (not the pitches -
                // touching the slide strip shouldn't flatten an authored pitch line), then un-arm.
                if (influenceArmed[ch])
                {
                    for (int s = 0; s < DrumChannel::MAX_STEPS; ++s)
                    {
                        slide[ch][s] = slide[ch][step];
                        if (onStepSlideChanged) onStepSlideChanged(ch, s, slide[ch][step]);
                    }
                    influenceArmed[ch] = false;
                    if (onInfluenceDisarm) onInfluenceDisarm(ch);
                }
                repaint();
                return;   // no drag from a slide toggle
            }
        }
        if (onStep)
        {
            // A MERGED (continuation) step has no values of its own - it belongs to its chain
            // HEAD. Magnify AND edit the head, so the magnified cell is ALWAYS the edited one.
            while (step > 0 && merge[ch][step]) --step;
            auto hr = stepRect(ch, step);                      // anchor inside the head's own cell
            beginMagnify(ch, step, { juce::jlimit(hr.getX(), hr.getRight() - 1, e.getPosition().x),
                                     e.getPosition().y });
        }
        dragChannel = onStep ? ch : -1; dragStep = onStep ? step : -1; handleValueDrag(e.getPosition());
    }
}

void StepGridComponent::mouseDrag(const juce::MouseEvent& e)
{
    // PIANO-ROLL editor: SCROLL the pitch view by dragging the note-name column.
    if (drawMagCh >= 0 && prMode == 7)   // TUNE fader drag
    {
        const auto tr = prHdrTune(drawOverlayRect());
        const float tv = juce::jlimit(-50.0f, 50.0f,
            ((float)(e.getPosition().x - tr.getX()) / (float) juce::jmax(1, tr.getWidth())) * 100.0f - 50.0f);
        if (std::round(tv) != drawTune[drawMagCh])
        {
            drawTune[drawMagCh] = std::round(tv);
            if (onDrawTuneChanged) onDrawTuneChanged(drawMagCh, drawTune[drawMagCh]);
            repaint();
        }
        return;
    }
    if (drawMagCh >= 0 && prMode == 4)
    {
        const auto lane = prLane(drawOverlayRect());
        const float rowH2 = juce::jmax(1.0f, (lane.getHeight() * 0.5f - 4.0f) / (float) drawRange);
        drawViewCenter = juce::jlimit(-prViewClamp(), prViewClamp(),
                                      prScrollGrabC + (int) std::lround((float) (e.getPosition().y - prScrollGrabY) / rowH2));
        repaint();
        return;
    }
    // PENDING right-click that started to MOVE -> promote to a fresh marquee area-select.
    if (drawMagCh >= 0 && prMode == 8 && e.getDistanceFromDragStart() > 3) { prClearSel(); prMode = 5; }
    // MARQUEE select: stretch the rubber band (selection resolves on mouse-up).
    if (drawMagCh >= 0 && prMode == 5) { prMarqB = e.getPosition(); repaint(); return; }
    // GROUP MOVE: shift every selected note by the drag delta (columns snapped to the grid).
    if (drawMagCh >= 0 && prMode == 6)
    {
        const int ch2 = drawMagCh;
        const auto lane = prLane(drawOverlayRect());
        int dCol = prColAt(lane, e.getPosition().x) - prGrabDCol;
        if (drawGridDiv > 0) { const int cw = DrumChannel::DRAW_RES / drawGridDiv;
                               dCol = (int) std::lround((double) dCol / cw) * cw; }
        const int dSemi = yToDrawSemi(lane, e.getPosition().y, drawRange, drawViewCenter) - prGrabDSemi;
        for (int i = 0; i < drawNoteCount[ch2]; ++i)
            if (prSel[i])
            {
                auto& n = drawNotes[ch2][i];
                n.start = (int16_t) juce::jlimit(0, totalCols() - 1, (int) prOrigStart[i] + dCol);
                n.semi  = (int8_t)  juce::jlimit(-DrumChannel::PITCH_RANGE, DrumChannel::PITCH_RANGE, (int) prOrigSemi[i] + dSemi);
            }
        if (prIdx >= 0 && prIdx < drawNoteCount[ch2]) { drawReadSemi = drawNotes[ch2][prIdx].semi;
                                                        if (onRollPreview) onRollPreview((int) drawReadSemi); }
        pushNotes(ch2);
        repaint();
        return;
    }
    // PIANO-ROLL editor gestures (MOVE / RESIZE / CREATE-stretch a note).
    if (drawMagCh >= 0 && prMode != 0 && prIdx >= 0 && prIdx < drawNoteCount[drawMagCh])
    {
        const int ch2 = drawMagCh;
        const auto lane = prLane(drawOverlayRect());
        const int col = prColAt(lane, e.getPosition().x);
        auto& n = drawNotes[ch2][prIdx];
        const int RES = DrumChannel::DRAW_RES;
        if (prMode == 1)   // MOVE: pitch + time (start snaps to the grid; length may cross bar lines)
        {
            n.start = (int16_t) juce::jlimit(0, totalCols() - 1, prSnap(juce::jmax(0, col - prGrabDCol)));
            n.semi  = (int8_t) juce::jlimit(-DrumChannel::PITCH_RANGE, DrumChannel::PITCH_RANGE, yToDrawSemi(lane, e.getPosition().y, drawRange, drawViewCenter) - prGrabDSemi);
            drawReadSemi = n.semi;
            if (onRollPreview) onRollPreview((int) n.semi);   // hear the pitch as you drag
        }
        else               // RESIZE / CREATE-stretch: the right edge follows the cursor (end snaps UP;
        {                  // notes may SPAN bar lines - the note keeps sounding into the next bar)
            int end = col + 1;
            if (drawGridDiv > 0)
            {
                const int cw = RES / drawGridDiv;
                end = ((col / cw) + 1) * cw;                 // snap the end to the NEXT grid line
            }
            n.len = (int16_t) juce::jlimit(1, totalCols() - (int) n.start, end - (int) n.start);
        }
        pushNotes(ch2);
        repaint();
        return;
    }
    if (drawDragCh >= 0) { drawStrokeTo(drawDragCh, e.getPosition()); return; }   // roll row = pure note drawing
    if (e.mods.isRightButtonDown() || e.mods.isPopupMenu()) return;
    if (editMode == ModeSteps) { handleClick(e.getPosition(), false); return; }
    if (editMode == ModeProb)
    {
        if (condDragCh < 0) return;
        if (std::abs(e.getPosition().x - condDownX) > 4) condDragged = true;
        if (condDragged) {                                    // horizontal drag -> cycle length 1..5
            auto r = activeStepRect(condDragCh, condDragStep);   // magnified while held
            const float xn = juce::jlimit(0.0f, 0.999f, (float)(e.getPosition().x - r.getX()) / (float) juce::jmax(1, r.getWidth()));
            const int N = juce::jlimit(1, 5, 1 + (int)(xn * 5.0f));
            if (N != condLen[condDragCh][condDragStep]) {
                condLen[condDragCh][condDragStep] = N;
                if (onStepCondChanged) onStepCondChanged(condDragCh, condDragStep, N, condMask[condDragCh][condDragStep]);
                notifyMag();
            }
        }
        return;
    }
    handleValueDrag(e.getPosition());
}

void StepGridComponent::mouseUp(const juce::MouseEvent&)
{
    if (onRollPreviewEnd) onRollPreviewEnd();   // any gesture end releases the drawing-audition note
    if (drawDragCh >= 0 && strokeCreatedNew && strokeNoteIdx >= 0)
    { tapNoteCh = drawDragCh; tapNoteIdx = strokeNoteIdx; }   // a click just created this note
    else if (drawDragCh >= 0) { tapNoteCh = -1; tapNoteIdx = -1; }

    if (prMode == 5 && drawMagCh >= 0)   // MARQUEE released: select every note inside the band
    {
        const int ch2 = drawMagCh;
        const auto lane = prLane(drawOverlayRect());
        const auto rect = juce::Rectangle<int>::leftTopRightBottom(juce::jmin(prMarqA.x, prMarqB.x), juce::jmin(prMarqA.y, prMarqB.y),
                                                                   juce::jmax(prMarqA.x, prMarqB.x), juce::jmax(prMarqA.y, prMarqB.y));
        const int c0 = prColAt(lane, rect.getX()), c1 = prColAt(lane, rect.getRight());
        const int sHi = yToDrawSemi(lane, rect.getY(), drawRange, drawViewCenter);      // top = higher pitch
        const int sLo = yToDrawSemi(lane, rect.getBottom(), drawRange, drawViewCenter);
        prClearSel();
        for (int i = 0; i < drawNoteCount[ch2]; ++i)
        {
            const auto& n = drawNotes[ch2][i];
            if (n.start < c1 && n.start + n.len > c0 && (int) n.semi >= sLo && (int) n.semi <= sHi)
            { prSel[i] = true; ++prSelCount; }
        }
        prMode = 0; repaint(); return;
    }
    if (prMode == 8)   // a right-click that DID NOT drag -> the note menu (on a note), else clear selection
    {
        const int ch2 = drawMagCh, ridx = prRightIdx;
        prMode = 0; prRightIdx = -1;
        if (ridx >= 0) showRollNoteMenu(ch2, ridx);
        else { prClearSel(); repaint(); }
        return;
    }
    if (prMode != 0) { prMode = 0; prIdx = -1; repaint(); return; }   // piano-roll gesture ended
    if (drawDragCh >= 0) { drawDragCh = -1; drawLastCol = -1; strokeNoteIdx = -1; strokeLockSemi = -128; drawReadSemi = -128; repaint(); return; }   // line stroke ended
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
    endMagnify();   // shrink the held step back
}



// The piano-roll NOTE right-click menu (Gate/Strum/Velocity/Pan/Glide/Slot/Delete). Called from
// mouseUp when a right-click did NOT drag (a right-DRAG is the marquee area-select instead).
void StepGridComponent::showRollNoteMenu(int ch2, int idx)
{
    if (idx < 0 || idx >= drawNoteCount[ch2]) return;
    auto deleteSelected = [this, ch2] {
        for (int i = drawNoteCount[ch2] - 1; i >= 0; --i)
            if (prSel[i]) { for (int j = i; j < drawNoteCount[ch2] - 1; ++j) drawNotes[ch2][j] = drawNotes[ch2][j + 1];
                            --drawNoteCount[ch2]; }
        prClearSel(); pushNotes(ch2); repaint();
    };
    const bool sel = prSel[idx];
    const auto& nn = drawNotes[ch2][idx];
    // STRUM only does something on stacked chord VOICES that share a time span. It's
    // available when either (user spec):
    //  - the target notes SHARE start AND length (a chord drawn by hand - ANY pitches), OR
    //  - a SINGLE note whose SOUND voices it into a chord (Scale/Chord = it has "shadow" notes).
    bool strumOk;
    {
        int fs = -1, fl = -1, count = 0; bool uniform = true;
        for (int i = 0; i < drawNoteCount[ch2]; ++i)
            if (i == idx || (sel && prSel[i])) {
                if (count == 0) { fs = drawNotes[ch2][i].start; fl = drawNotes[ch2][i].len; }
                else if (drawNotes[ch2][i].start != fs || drawNotes[ch2][i].len != fl) uniform = false;
                ++count;
            }
        auto hasShadows = [&](const DrumChannel::DrawNote& q) -> bool {
            if (! getSlotVoicing) return false; int vo[8];
            if ((q.slot == 0 || q.slot == 1) && getSlotVoicing(ch2, 0, q.semi, vo) > 1) return true;
            if ((q.slot == 0 || q.slot == 2) && getSlotVoicing(ch2, 1, q.semi, vo) > 1) return true;
            return false;
        };
        strumOk = uniform && (count >= 2 || (count == 1 && hasShadows(drawNotes[ch2][idx])));
    }
    juce::PopupMenu m;
    m.addSectionHeader(sel ? "Selected notes" : "Note");
    m.addItem(1, "Gate OFF: ring naturally (like a step)", true, nn.oneShot != 0);
    m.addItem(2, "Gate ON: hold for the note length",       true, nn.oneShot == 0);
    m.addSeparator();
    m.addItem(5, "Strum DOWN (normal)",            strumOk, nn.strumUp == 0);
    m.addItem(6, "Strum UP (alt. strum: reversed + lighter)", strumOk, nn.strumUp != 0);
    juce::PopupMenu sa;   // per-note strum AMOUNT override; stepped 0/20/40/60/80/100 (matches the knob)
    sa.addItem(20, "Sound's Strum knob (default)", strumOk, nn.strumPct == 255);
    sa.addItem(21, "0% (no strum on this note)",   strumOk, nn.strumPct == 0);
    sa.addItem(22, "20%",                          strumOk, nn.strumPct == 20);
    sa.addItem(23, "40%",                          strumOk, nn.strumPct == 40);
    sa.addItem(24, "60%",                          strumOk, nn.strumPct == 60);
    sa.addItem(28, "80%",                          strumOk, nn.strumPct == 80);
    sa.addItem(29, "100%",                         strumOk, nn.strumPct == 100);
    m.addSubMenu("Strum amount (needs a chord: stacked same-time notes, or a Scale/Chord sound)", sa, strumOk);
    m.addSeparator();
    // VELOCITY: "Type exact %" gives FULL 1% resolution (matches the min/max vel knobs -
    // no coarse mismatch, user rule), plus quick 10% presets. Current % shown at the top.
    juce::PopupMenu vm;
    vm.addItem(31, "Type exact % (currently " + juce::String((int) std::lround((float) nn.vel / 255.0f * 100.0f)) + "%)...");
    vm.addSeparator();
    for (int vp = 10; vp <= 100; vp += 10) {
        const int v255 = juce::jlimit(1, 255, (int) std::lround(vp / 100.0 * 255.0));
        vm.addItem(300 + vp, juce::String(vp) + "%", true, std::abs((int) nn.vel - v255) <= 12);
    }
    m.addSubMenu("Velocity", vm);
    // PAN (per note). Default = follow the whole-channel pan (inherit); the explicit
    // positions override it (0 = a true centre, distinct from inherit).
    const bool panInherit = nn.pan == DrumChannel::PAN_INHERIT;
    juce::PopupMenu pm;
    pm.addItem(205, "Channel pan (default)", true, panInherit);
    pm.addSeparator();
    pm.addItem(200, "Left 100%",  true, ! panInherit && nn.pan <= -90);
    pm.addItem(201, "Left 50%",   true, ! panInherit && nn.pan > -90 && nn.pan <= -30);
    pm.addItem(202, "Centre",     true, ! panInherit && nn.pan > -30 && nn.pan <  30);
    pm.addItem(203, "Right 50%",  true, ! panInherit && nn.pan >= 30 && nn.pan <  90);
    pm.addItem(204, "Right 100%", true, ! panInherit && nn.pan >= 90);
    m.addSubMenu("Pan", pm);
    m.addSeparator();
    m.addItem(3, "Glide into this note", true, nn.glide != 0);
    juce::PopupMenu sm;
    sm.addItem(10, "Both slots", true, nn.slot == 0);
    sm.addItem(11, "Slot 1",     true, nn.slot == 1);
    sm.addItem(12, "Slot 2",     true, nn.slot == 2);
    m.addSubMenu("Play on", sm);
    m.addSeparator();
    m.addItem(4, sel ? "Delete selected notes" : "Delete note");
    m.showMenuAsync(juce::PopupMenu::Options(),
        [this, ch2, idx, sel, deleteSelected](int r)
        {
            if (r == 0 || idx >= drawNoteCount[ch2]) return;
            auto apply = [&](void (*f)(DrumChannel::DrawNote&, int), int arg)
            {
                for (int i = 0; i < drawNoteCount[ch2]; ++i)
                    if (i == idx || (sel && prSel[i])) f(drawNotes[ch2][i], arg);
                pushNotes(ch2); repaint();
            };
            if (r == 1)      apply(+[](DrumChannel::DrawNote& n, int){ n.oneShot = 1; }, 0);
            else if (r == 2) apply(+[](DrumChannel::DrawNote& n, int){ n.oneShot = 0; }, 0);
            else if (r == 3) apply(+[](DrumChannel::DrawNote& n, int){ n.glide = n.glide ? 0 : 1; }, 0);
            else if (r == 5) apply(+[](DrumChannel::DrawNote& n, int){ n.strumUp = 0; }, 0);
            else if (r == 6) apply(+[](DrumChannel::DrawNote& n, int){ n.strumUp = 1; }, 0);
            else if (r == 20) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 255; }, 0);
            else if (r == 21) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 0;   }, 0);
            else if (r == 22) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 20;  }, 0);
            else if (r == 23) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 40;  }, 0);
            else if (r == 24) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 60;  }, 0);
            else if (r == 28) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 80;  }, 0);
            else if (r == 29) apply(+[](DrumChannel::DrawNote& n, int){ n.strumPct = 100; }, 0);
            else if (r == 205) apply(+[](DrumChannel::DrawNote& n, int){ n.pan = DrumChannel::PAN_INHERIT; }, 0);
            else if (r >= 200 && r <= 204) { const int pv[5] = { -100, -50, 0, 50, 100 };
                                             apply(+[](DrumChannel::DrawNote& n, int a){ n.pan = (int8_t) a; }, pv[r - 200]); }
            else if (r >= 310 && r <= 400) apply(+[](DrumChannel::DrawNote& n, int a){ n.vel = (uint8_t) a; },
                                                 juce::jlimit(1, 255, (int) std::lround((r - 300) / 100.0 * 255.0)));
            else if (r == 31)   // TYPE EXACT velocity % - full 1% resolution (matches the knobs)
            {
                const int cur = (int) std::lround((float) drawNotes[ch2][idx].vel / 255.0f * 100.0f);
                auto* aw = new juce::AlertWindow("Note velocity", "Velocity % (0-100):",
                                                 juce::MessageBoxIconType::NoIcon);
                aw->addTextEditor("v", juce::String(cur));
                aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
                aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                aw->enterModalState(true, juce::ModalCallbackFunction::create(
                    [this, ch2, idx, sel, aw](int rr) {
                        if (rr != 1) return;
                        const int pct  = juce::jlimit(0, 100, aw->getTextEditorContents("v").getIntValue());
                        const int v255 = juce::jlimit(1, 255, (int) std::lround(pct / 100.0 * 255.0));
                        for (int i = 0; i < drawNoteCount[ch2]; ++i)
                            if (i == idx || (sel && prSel[i])) drawNotes[ch2][i].vel = (uint8_t) v255;
                        pushNotes(ch2); repaint();
                    }), true);
            }
            else if (r >= 10 && r <= 12) apply(+[](DrumChannel::DrawNote& n, int a){ n.slot = (uint8_t) a; }, r - 10);
            else if (r == 4)
            {
                if (sel) { deleteSelected(); return; }
                prClearSel();
                for (int j = idx; j < drawNoteCount[ch2] - 1; ++j) drawNotes[ch2][j] = drawNotes[ch2][j + 1];
                --drawNoteCount[ch2]; pushNotes(ch2); repaint();
            }
        });
}
