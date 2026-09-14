#include "PluginEditor.h"

void KeysPanel::setDrumming(bool on)
{
    if (drummingUi == on)
        return;
    const int recordMode = juce::jmax(1, comboRecMode.getSelectedId());
    drummingUi = on;
    comboRecMode.changeItemText(1, on ? "This pattern - first pad" : "This pattern only - key");
    comboRecMode.changeItemText(2, on ? "This pattern - 3s count-in" : "This pattern only - 3s count-in");
    comboRecMode.changeItemText(3, on ? "Follow chain - first pad" : "Follow chain, record each - key");
    comboRecMode.changeItemText(4, on ? "Follow chain - 3s count-in" : "Follow chain, record each - 3s");
    comboRecMode.setSelectedId(recordMode, juce::dontSendNotification);
    for (int i = 0; i < getNumChildComponents(); ++i)
    {
        auto *c = getChildComponent(i);
        const bool active = !on || c == &btnRec || c == &comboRecMode || c == &btnTakes || c == &lblRecMode;
        c->setEnabled(active);
        c->setAlpha(active ? 1.0f : 0.35f);
    }
    if (on)
    {
        arpEditor.setVisible(false);
        held.clear();
        kbState.allNotesOff(0);
    }
}

void DrumSequencerEditor::setupLiveDrumming()
{
    content.addChildComponent(drumGrid);
    content.addChildComponent(drumModePrompt);
    for (auto *b : {&btnLiveDrumming, &btnWindowMinus, &btnWindowPlus, &btnDrumQuantize})
    {
        content.addAndMakeVisible(b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20203a));
    }
    btnLiveDrumming.setTooltip(
        "LIVE DRUMMING: MIDI notes trigger their assigned sound channels regardless of which channel you are "
        "editing. Assign pads in Routing > Channel > MIDI In (Learn). The common drum roll has one row per "
        "channel; KEYS/RECORD records the whole kit. Keyboard performance controls, Merge & Split, Generate "
        "and step editing are unavailable in this mode. Switching offers Keep and convert, Start fresh, or "
        "Cancel, with an explanation of what changes.");
    btnLiveDrumming.onClick = [this] { requestDrummingSwitch(); };
    btnWindowMinus.setTooltip("Make the entire plugin window smaller (5% of design size). Size is remembered "
                              "in the project. Use this if your host's window frame clips the plugin.");
    btnWindowPlus.setTooltip(
        "Make the entire plugin window larger (5% of design size), up to the available display area. Size is "
        "remembered in the project. The corner resize handle also works.");
    btnWindowMinus.onClick = [this] { applyWindowScale(proc.editorScale - 0.05); };
    btnWindowPlus.onClick = [this] { applyWindowScale(proc.editorScale + 0.05); };
    content.addChildComponent(drumSnap);
    content.addChildComponent(lblDrumStatus);
    drumSnap.addItem("Snap: Off", 1);
    for (int n : {4, 8, 12, 16, 24, 32, 48, 64})
        drumSnap.addItem("Snap: 1/" + juce::String(n), n + 1);
    drumSnap.setSelectedId(17, juce::dontSendNotification);
    drumSnap.setTooltip(
        "Snap only affects added or moved hits. Live recording always retains the incoming timing. Quantize "
        "applies this grid to existing hits in the displayed pattern/group.");
    drumSnap.onChange = [this]
    {
        drumGrid.snap = drumSnap.getSelectedId() - 1;
        drumGrid.repaint();
    };
    btnDrumQuantize.setTooltip("Snap all drum hits in the displayed pattern or merged group to the selected "
                               "grid. Velocity, instrument and pan stay unchanged. Undo restores the "
                               "previous timing. Unavailable while recording or with Snap Off.");
    btnDrumQuantize.onClick = [this] { drumGrid.quantize(); };
    lblDrumStatus.setColour(juce::Label::textColourId, juce::Colour(0xff8fdfbf));
    lblDrumStatus.setFont(juce::Font(12));
    lblDrumStatus.setTooltip("Incoming pad note, MIDI channel and velocity. Unassigned notes do not sound. "
                             "To assign one, open Routing > Channel > MIDI In > Learn, then strike the pad. "
                             "Learning consumes that strike; strike again to play.");
    drumGrid.beforeEdit = [this] { commitUndoNow(); };
    drumGrid.selectChannel = [this](int ch) { selectChannel(ch); };
    drumModePrompt.onChoice = [this](int choice)
    {
        if (choice == 0)
        {
            drumModePrompt.setVisible(false);
            return;
        }
        commitUndoNow();
        juce::String error;
        if (!proc.switchDrumming(drumModePrompt.destinationDrums, choice == 1, error))
        {
            drumModePrompt.error = error;
            drumModePrompt.repaint();
            return;
        }
        drumModePrompt.setVisible(false);
        keysLoadedTakeIdx = -1;
        drumLastTake = -1;
        keysUiHash = -999;
        fullRefresh();
        drumGrid.update();
        refreshLiveDrumming();
        layoutContent();
        commitUndoNow();
    };
}
void DrumSequencerEditor::requestDrummingSwitch()
{
    if (proc.keysRecording.load() || proc.sequencer.drums.recording || drumCountdown > 0 ||
        keysCountdownTicks > 0)
        return;
    drumModePrompt.destinationDrums = !proc.sequencer.drums.enabled;
    drumModePrompt.error.clear();
    drumModePrompt.setVisible(true);
    drumModePrompt.toFront(false);
}
void DrumSequencerEditor::refreshLiveDrumming()
{
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &d = proc.sequencer.drums;
    btnLiveDrumming.setColour(juce::TextButton::buttonColourId,
                              juce::Colour(d.enabled ? 0xff246952 : 0xff20203a));
    btnLiveDrumming.setEnabled(!d.recording && !proc.keysRecording.load() && drumCountdown == 0 &&
                               keysCountdownTicks == 0);
    if (d.enabled && regularRecTip.isEmpty())
    {
        regularRecTip = keysPanel.btnRec.getTooltip();
        regularRecModeTip = keysPanel.comboRecMode.getTooltip();
        regularTakesTip = keysPanel.btnTakes.getTooltip();
        regularClearTip = btnClearPat.getTooltip();
        regularDragTip = dragMidi.getTooltip();
    }
    else if (!d.enabled && regularRecTip.isNotEmpty())
    {
        keysPanel.btnRec.setTooltip(regularRecTip);
        keysPanel.comboRecMode.setTooltip(regularRecModeTip);
        keysPanel.btnTakes.setTooltip(regularTakesTip);
        btnClearPat.setTooltip(regularClearTip);
        dragMidi.setTooltip(regularDragTip);
        regularRecTip.clear();
    }
    keysPanel.setDrumming(d.enabled);
    comboPreset.setEnabled(!d.recording && drumCountdown == 0);
    btnPause.setEnabled(!d.recording && drumCountdown == 0 && !proc.keysRecording.load() &&
                        keysCountdownTicks == 0);
    drumGrid.setEnabled(!d.recording && drumCountdown == 0);
    for (auto &st : strips)
        if (d.enabled)
        {
            st.comboSteps.setEnabled(false);
            st.comboSteps.setAlpha(0.35f);
        }
    btnDrumQuantize.setEnabled(d.enabled && !d.recording && drumCountdown == 0 && drumGrid.snap > 0);
    btnClearPat.setEnabled(!d.enabled || (!d.recording && drumCountdown == 0));
    sliderSwing.setEnabled(!d.enabled);
    sliderSwing.setAlpha(d.enabled ? 0.35f : 1.0f);
    lblSwing.setAlpha(d.enabled ? 0.35f : 1.0f);
    if (!d.enabled)
        return;
    btnUndo.setEnabled(!d.recording && drumCountdown == 0 && undoStack.size() >= 2);
    btnRedo.setEnabled(!d.recording && drumCountdown == 0 && !redoStack.empty());
    drumModePrompt.keep.setEnabled(!d.recording);
    const bool rec = d.recording || drumCountdown > 0;
    const int head = proc.sequencer.groupHead(currentPattern()), end = proc.sequencer.groupEnd(head);
    int count = 0;
    for (const auto &t : d.takes)
        if (t.head >= head && t.head <= end)
            ++count;
    keysPanel.btnRec.setButtonText(rec ? "STOP REC" : "REC KIT");
    keysPanel.btnRec.setColour(juce::TextButton::buttonColourId, juce::Colour(rec ? 0xffbb2828 : 0xff20203a));
    keysPanel.btnRec.setEnabled(rec || (d.takesFor(head) < 20 && d.takes.size() < LiveDrumming::MAX_TAKES));
    keysPanel.comboRecMode.setEnabled(!rec);
    keysPanel.btnTakes.setEnabled(!rec);
    keysPanel.btnTakes.setButtonText("Takes (" + juce::String(count) + ")");
    btnClearPat.setTooltip("Clear ALL drum channels in the displayed pattern/merged group. Saved takes "
                           "remain available. Undo restores the cleared hits. Unavailable while recording.");
    dragMidi.setTooltip("Drag to your DAW to export ALL drum channels in the displayed pattern/group as one "
                        "MIDI clip. Hits use their MIDI In assignments (or the MIDI Out note when "
                        "unassigned), preserving rhythm and velocity.");
    keysPanel.btnRec.setTooltip(
        "Record ALL assigned drum channels together, including channels outside the visible rows. This "
        "pattern: each full pass of the pattern/merged group becomes a kit take. Follow chain: each visited "
        "bar becomes a kit take. A new pass starts fresh; no overdub. Recording does not quantize your "
        "timing. Press again to stop and keep the latest take. Sound selection never changes MIDI routing.");
    keysPanel.comboRecMode.setTooltip(
        "This pattern: loop the selected pattern/group while recording the whole kit. Follow chain: record "
        "each visited bar using its own sounds. 'pad' means the first assigned pad starts BASAMAK's stopped "
        "transport; '3s' gives a three-second count-in. With DAW Sync enabled, start playback in your DAW; "
        "BASAMAK never starts the host.");
    keysPanel.btnTakes.setTooltip(
        "Kit-wide takes for this pattern/group, independent of channel selection. Loading replaces the whole "
        "kit's notes in that take's bars, after confirmation. You can save edits as a new take, rename "
        "takes, or delete them. Up to 20 takes per pattern/group and 1000 per preset.");
}
void DrumSequencerEditor::startDrumRecord()
{
    commitUndoNow();
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &s = proc.sequencer;
    auto &d = s.drums;
    if (d.takes.size() >= LiveDrumming::MAX_TAKES || d.takesFor(s.groupHead(currentPattern())) >= 20)
        return;
    drumStopSerial = proc.transportStopSerial.load();
    if (s.isCurrentlyPlaying)
        selectPattern(s.playPattern);
    const int head = s.groupHead(currentPattern()), end = s.groupEnd(head);
    int mode = juce::jmax(0, keysPanel.comboRecMode.getSelectedId() - 1);
    d.arm(head, end, mode >= 2);
    drumLastTake = (int)d.takes.size();
    drumRecWasPlaying = false;
    if (mode % 2 == 1)
    {
        d.recording = false;
        drumCountdown = 180;
    }
    else
        drumCountdown = 0;
    proc.followPlayback = true;
    refreshFollowButton();
    refreshLiveDrumming();
}
void DrumSequencerEditor::stopDrumRecord()
{
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &d = proc.sequencer.drums;
    const bool wasCountIn = drumCountdown > 0;
    drumCountdown = 0;
    drumGoTicks = 0;
    d.stopRecording();
    d.drainLog();
    proc.sequencer.recordLoopLock.store(false);
    if (!wasCountIn && drumLastTake >= 0 && (int)d.takes.size() > drumLastTake)
    {
        d.loadTake((int)d.takes.size() - 1);
        selectPattern(d.takes.back().head);
    }
    drumLastTake = -1;
    drumRecWasPlaying = false;
    countdownOverlay.label.clear();
    countdownOverlay.repaint();
    refreshLiveDrumming();
    drumGrid.update();
}
void DrumSequencerEditor::tickLiveDrumming()
{
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &s = proc.sequencer;
    auto &d = s.drums;
    if (lastDrumEnabled != d.enabled)
    {
        lastDrumEnabled = d.enabled;
        drumCountdown = 0;
        drumLastTake = -1;
        refreshDrawModeButtons();
        refreshKeysPanel();
        layoutContent();
    }
    if (!d.enabled)
        return;
    if (drumLastTake >= 0 && drumStopSerial != proc.transportStopSerial.load())
    {
        stopDrumRecord();
        return;
    }
    if (drumCountdown > 0 && --drumCountdown == 0)
    {
        d.recording = true;
        drumGoTicks = 42;
        if (!s.dawSync && !s.isCurrentlyPlaying)
            s.startStandalone();
    }
    else if (drumGoTicks > 0)
        --drumGoTicks;
    if (drumLastTake >= 0)
    {
        d.drainLog();
        if (s.isCurrentlyPlaying && d.recording)
            drumRecWasPlaying = true;
        if (drumCountdown == 0 && ((drumRecWasPlaying && !s.isCurrentlyPlaying) || !d.recording))
            stopDrumRecord();
    }
    const juce::String cd =
        drumCountdown > 0 ? juce::String((drumCountdown + 59) / 60) : (drumGoTicks > 0 ? "GO!" : "");
    if (countdownOverlay.label != cd)
    {
        countdownOverlay.label = cd;
        countdownOverlay.repaint();
    }
    juce::String status = "Assign pads in Routing > MIDI In";
    if (d.learnChannel >= 0)
        status = "LEARN Ch " + juce::String(d.learnChannel + 1) + ": strike a pad (Routing to cancel)";
    else if (d.lastNote >= 0)
        status = "Note " + juce::String(d.lastNote) + " | MIDI ch " + juce::String(d.lastMidiChannel) +
                 " | Vel " + juce::String(d.lastVelocity) +
                 (d.target(d.lastNote, d.lastMidiChannel) < 0 ? " | Unassigned" : "");
    if (d.recordOverflow)
        status = "Recording limit reached. Your captured takes are kept.";
    else if (d.inputOverflow)
        status = "MIDI input limit reached; some hits were dropped.";
    if (!lblDrumStatus.isBeingEdited())
        lblDrumStatus.setText(status, juce::dontSendNotification);
    refreshLiveDrumming();
    drumGrid.update();
}
void DrumSequencerEditor::showDrumTakes()
{
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &d = proc.sequencer.drums;
    if (d.recording || drumCountdown > 0)
        return;
    int head = proc.sequencer.groupHead(currentPattern());
    juce::PopupMenu m;
    m.addSectionHeader("Kit takes - all channels (Bar " + juce::String(head + 1) + ")");
    m.addItem(1, "Save current kit notes as a new take",
              d.takesFor(head) < 20 && d.takes.size() < LiveDrumming::MAX_TAKES);
    for (int i = 0; i < (int)d.takes.size(); ++i)
        if (d.takes[(size_t)i].head >= head && d.takes[(size_t)i].head <= proc.sequencer.groupEnd(head))
        {
            juce::PopupMenu sub;
            sub.addItem(100 + i, "Load (replace kit notes)");
            sub.addItem(2000 + i, "Rename");
            sub.addItem(4000 + i, "Delete take");
            m.addSubMenu(d.takes[(size_t)i].name + " (Bar " + juce::String(d.takes[(size_t)i].head + 1) + ")",
                         sub);
        }
    juce::Component::SafePointer<DrumSequencerEditor> safe(this);
    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&keysPanel.btnTakes),
        [safe, head](int r)
        {
            if (!safe || r == 0)
                return;
            auto &o = *safe;
            auto &d = o.proc.sequencer.drums;
            if (!d.enabled || d.recording || o.drumCountdown > 0) return;
            if (r == 1)
            {
                o.commitUndoNow();
                const juce::ScopedLock guard(o.proc.getCallbackLock());
                LiveDrumming::Take t;
                t.head = head;
                t.bars = o.proc.sequencer.groupEnd(head) - head + 1;
                t.name = "Kit take " + juce::String((int)d.takes.size() + 1);
                for (int p = head; p < head + t.bars; ++p)
                {
                    auto &l = d.patterns[(size_t)p];
                    for (int i = 0; i < l.count; ++i)
                        t.hits.push_back({p, l.hits[(size_t)i]});
                }
                if (!t.hits.empty() && d.takesFor(head) < 20 && d.takes.size() < LiveDrumming::MAX_TAKES)
                    d.takes.push_back(std::move(t));
                o.refreshLiveDrumming();
                return;
            }
            const int idx = r >= 4000 ? r - 4000 : r >= 2000 ? r - 2000 : r - 100;
            if (idx < 0 || idx >= (int)d.takes.size())
                return;
            if (r >= 2000 && r < 4000)
            {
                // In-editor text entry, avoiding native/modal dialogs in plugin hosts.
                o.lblDrumStatus.setEditable(false, true, false);
                o.lblDrumStatus.setText(d.takes[(size_t)idx].name, juce::dontSendNotification);
                o.lblDrumStatus.onTextChange = [safe, idx]
                {
                    if (!safe)
                        return;
                    auto &e = *safe;
                    e.commitUndoNow();
                    const juce::ScopedLock l(e.proc.getCallbackLock());
                    auto &takes = e.proc.sequencer.drums.takes;
                    if (idx < (int)takes.size())
                    {
                        auto name = e.lblDrumStatus.getText().trim();
                        if (name.isNotEmpty())
                            takes[(size_t)idx].name = name;
                    }
                };
                o.lblDrumStatus.onEditorHide = [safe] {
                    if (safe) safe->lblDrumStatus.setEditable(false, false, false);
                };
                o.lblDrumStatus.showEditor();
                return;
            }
            juce::PopupMenu confirm;
            confirm.addSectionHeader(r >= 4000 ? "Delete this saved kit take?"
                                               : "Replace the kit notes in this take's bars?");
            confirm.addItem(1, r >= 4000 ? "Delete take" : "Load kit take");
            confirm.addItem(2, "Cancel");
            confirm.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&o.keysPanel.btnTakes),
                                  [safe, idx, r](int choice)
                                  {
                                      if (!safe || choice != 1)
                                          return;
                                      auto &e = *safe;
                                      e.commitUndoNow();
                                      const juce::ScopedLock l(e.proc.getCallbackLock());
                                      auto &d = e.proc.sequencer.drums;
                                      if (d.recording || idx >= (int)d.takes.size())
                                          return;
                                      if (r >= 4000)
                                          d.takes.erase(d.takes.begin() + idx);
                                      else if (d.loadTake(idx))
                                          e.selectPattern(d.takes[(size_t)idx].head);
                                      e.refreshLiveDrumming();
                                      e.drumGrid.update();
                                  });
        });
}
void DrumSequencerEditor::applyWindowScale(double requested)
{
    auto &displays = juce::Desktop::getInstance().getDisplays();
    auto *display = displays.getDisplayForRect(getScreenBounds());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    const auto area = display != nullptr ? display->userArea : juce::Rectangle<int>(0, 0, 1920, 1080);
    const double maximum =
        juce::jmax(0.4, juce::jmin(2.0, juce::jmin((area.getWidth() - 32.0) / DESIGN_W,
                                                   (area.getHeight() - 90.0) / contentHeightPx)));
    const double scale = juce::jlimit(0.4, maximum, requested);
    setResizeLimits((int)(DESIGN_W * 0.4), (int)(contentHeightPx * 0.4), (int)(DESIGN_W * maximum),
                    (int)(contentHeightPx * maximum));
    if (auto *bounds = getConstrainer())
        bounds->setFixedAspectRatio((double)DESIGN_W / contentHeightPx);
    proc.editorScale = scale;
    setSize(juce::roundToInt(DESIGN_W * scale), juce::roundToInt(contentHeightPx * scale));
}
