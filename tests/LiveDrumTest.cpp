#include "PluginEditor.h"
#include <cmath>
#include <cstdio>
#include <memory>

static int fails = 0;
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            printf("FAIL %d: %s\n", __LINE__, #x);                                                           \
            ++fails;                                                                                         \
        }                                                                                                    \
    } while (false)
static void tone(DrumChannel &c, float hz)
{
    for (auto &sl : c.slots)
        sl = DrumChannel::Slot();
    auto &sl = c.slots[0];
    sl.engine = DrumChannel::SrcOsc;
    sl.weight = 1;
    sl.oscFreq = hz;
    sl.oscShape = 0;
    sl.oscUnison = 1;
    sl.oscDetune = 0;
    sl.sustain = 0;
    sl.atk = 0;
    sl.hold = 0;
    sl.dec = 0.3f;
    c.restoredSlots = true;
    c.volume = 0.5f;
    c.allowOverlap = true;
    c.markDspDirty();
}
static juce::Button *button(juce::Component &c, const juce::String &text)
{
    if (auto *b = dynamic_cast<juce::Button *>(&c))
        if (b->getButtonText() == text)
            return b;
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        if (auto *b = button(*c.getChildComponent(i), text))
            return b;
    return nullptr;
}
static KeysPanel *keysPanel(juce::Component &c)
{
    if (auto *p = dynamic_cast<KeysPanel *>(&c))
        return p;
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        if (auto *p = keysPanel(*c.getChildComponent(i)))
            return p;
    return nullptr;
}
static void snapshot(juce::Component &c, const juce::String &name)
{
    auto im = c.createComponentSnapshot(c.getLocalBounds());
    juce::File f("/private/tmp/" + name + ".png");
    f.deleteFile();
    juce::FileOutputStream out(f);
    juce::PNGImageFormat png;
    png.writeImageToStream(im, out);
}
struct TestPlayHead : juce::AudioPlayHead
{
    double ppq = 0;
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setPpqPosition(ppq);
        p.setBpm(120);
        p.setTimeSignature(TimeSignature{4, 4});
        p.setIsPlaying(playing);
        return p;
    }
};
int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    printf("Live drumming: mapping, timestamps, takes, persistence, conversion and UI\n");
    {
        auto d = std::make_unique<LiveDrumming>();
        CHECK(d->target(36, 10) == 0);
        CHECK(d->target(38, 10) == 1);
        CHECK(d->target(127, 10) == -1);
        d->assign(15, 36, 10);
        CHECK(d->target(36, 10) == 15);
        CHECK(d->target(36, 1) == -1);
        d->learnChannel = 3;
        d->noteOn(80, 7, 31, 99);
        CHECK(d->notes[3] == 80 && d->midiChannels[3] == 7 && d->inputCount == 0);
        d->noteOn(80, 7, 31, 99);
        CHECK(d->inputCount == 1 && d->input[0].offset == 99 && d->input[0].channel == 3);
        d->arm(0, 1, false);
        d->enterSegment(0, 0);
        d->record(0, {0.1, 0, 0.2f, 0});
        d->enterSegment(1, 1);
        d->record(1, {0.4, 15, 0.8f, 0.5f});
        d->enterSegment(0, 2);
        d->drainLog();
        CHECK(d->takes.size() == 1 && d->takes[0].hits.size() == 2 && d->takes[0].bars == 2);
        CHECK(d->patterns[0].count == 0 && d->patterns[1].count == 0);
        d->stopRecording();
        d->drainLog();
        CHECK(d->takes.size() == 1);
        CHECK(d->loadTake(0));
        CHECK(d->patterns[1].count == 1);
        d->enabled = true;
        auto saved = d->save();
        d->restore(saved);
        CHECK(d->enabled && d->takes.size() == 1 && d->notes[3] == 80);
        CHECK(d->patterns[1].hits[0].channel == 15);
        d->restore({});
        CHECK(!d->enabled && d->takes.empty() && d->notes[0] == 36);
    }
    {
        auto s = std::make_unique<Sequencer>();
        s->drums.enabled = true;
        for (int p = 0; p < 2; ++p)
            for (int c = 0; c < 16; ++c)
            {
                tone(s->patterns[p].channels[c], 100.0f + c * 31);
                s->patterns[p].channels[c].prepareToPlay(48000, 512);
            }
        juce::AudioBuffer<float> b(2, 512);
        s->drums.beginBlock();
        for (int c = 0; c < 8; ++c)
        {
            s->drums.assign(c, 60 + c, 10);
            s->drums.noteOn(60 + c, 10, 20 + c * 10, 111);
        }
        b.clear();
        auto ev = s->processBlock(b, 48000, 512, nullptr);
        CHECK(ev.size() == 8);
        for (auto e : ev)
            CHECK(e.offset == 111 && e.drumHit && e.pattern == 0);
        CHECK(b.getMagnitude(0, 0, 111) == 0);
        CHECK(b.getMagnitude(0, 112, 400) > 0.001f);
        s->drums.beginBlock();
        b.clear();
        s->processBlock(b, 48000, 512, nullptr);
        CHECK(b.getMagnitude(0, 0, 512) > 0.001f); // natural tails without held keys
        s->stopStandalone();
        s->reset();
        s->drums.arm(0, 0, false);
        s->recordLoopLock.store(true);
        s->startStandalone();
        s->drums.beginBlock();
        s->drums.noteOn(60, 10, 73, 237);
        b.clear();
        ev = s->processBlock(b, 48000, 512, nullptr);
        CHECK(s->drums.patterns[0].count == 1);
        CHECK(std::abs(s->drums.patterns[0].hits[0].pos - 237.0 / 96000) < 1e-10);
        CHECK(std::abs(s->drums.patterns[0].hits[0].velocity - 73.0 / 127) < 1e-7);
        s->drums.stopRecording();
        s->drums.drainLog();
        CHECK(s->drums.takes.size() == 1);
        s->stopStandalone();
        s->reset();
        s->startStandalone();
        s->drums.beginBlock();
        b.clear();
        ev = s->processBlock(b, 48000, 512, nullptr);
        CHECK(ev.size() == 1 && ev[0].offset == 237);
        // A block crossing into the next bar records each hit in the correct source bar.
        s->drums.clearNotes();
        s->drums.arm(0, 1, false);
        s->patterns[1].mergeWithPrev = true;
        s->stopStandalone();
        s->reset();
        s->startStandalone();
        for (int i = 0; i < 187; ++i)
        {
            s->drums.beginBlock();
            b.clear();
            s->processBlock(b, 48000, 512, nullptr);
        } // 95744 frames; boundary 256 frames into next block
        s->drums.beginBlock();
        s->drums.noteOn(60, 10, 100, 100);
        s->drums.noteOn(61, 10, 90, 400);
        b.clear();
        ev = s->processBlock(b, 48000, 512, nullptr);
        CHECK(ev.size() == 2 && ev[0].pattern == 0 && ev[1].pattern == 1 && ev[0].offset == 100 &&
              ev[1].offset == 400);
        CHECK(s->drums.patterns[0].count == 1 && s->drums.patterns[1].count == 1);
    }
    {
        auto s = std::make_unique<Sequencer>();
        s->drums.enabled = true;
        auto &c = s->patterns[0].channels[15];
        tone(c, 800);
        c.outputBus = 1;
        c.prepareToPlay(48000, 512);
        s->drums.assign(15, 90, 10);
        s->drums.noteOn(90, 10, 100, 123);
        juce::AudioBuffer<float> main(2, 512), aux(2, 512);
        main.clear();
        aux.clear();
        juce::AudioBuffer<float> *buses[] = {&aux};
        auto events = s->processBlock(main, 48000, 512, nullptr, buses, 1);
        CHECK(events.size() == 1 && events[0].channel == 15 && events[0].offset == 123);
        CHECK(main.getMagnitude(0, 0, 512) == 0);
        CHECK(aux.getMagnitude(0, 0, 123) == 0);
        CHECK(aux.getMagnitude(0, 124, 388) > 0.001f);
    }
    {
        // Native-tuning conversion preserves audible oscillator output, not just stored settings.
        auto regular = std::make_unique<DrumChannel>(), drum = std::make_unique<DrumChannel>();
        tone(*regular, 57);
        tone(*drum, 57);
        regular->drawMode = false;
        drum->drawMode = true;
        regular->prepareToPlay(48000, 1024);
        drum->prepareToPlay(48000, 1024);
        regular->trigger(0.7f, 0, 0, 0);
        drum->trigger(0.7f, 0, 0, 0, 0, 0, false, 0, false, true);
        juce::AudioBuffer<float> a(2, 1024), b(2, 1024);
        a.clear();
        b.clear();
        regular->renderInto(a, 0, 1024, false);
        drum->renderInto(b, 0, 1024, false);
        CHECK(a.getMagnitude(0, 0, 1024) > 0.01f);
        CHECK(b.getMagnitude(0, 0, 1024) > 0.01f);
        float diff = 0;
        for (int i = 0; i < 1024; ++i)
            diff = std::max(diff, std::abs(a.getSample(0, i) - b.getSample(0, i)));
        CHECK(diff < 1.0e-5f);
    }
    {
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto &s = p->sequencer;
        juce::String error;
        auto &c = s.patterns[0].channels[0];
        tone(c, 57);
        c.drawMode = false;
        c.steps[3] = true;
        c.stepVel[3] = 0.4f;
        c.stepRoll[3] = 2;
        c.stepRollDecay[3] = -0.5f;
        CHECK(p->switchDrumming(true, true, error));
        CHECK(s.drums.enabled && s.drums.patterns[0].count == 2);
        CHECK(c.slots[0].oscFreq == 57);
        CHECK(std::abs(s.drums.patterns[0].hits[1].velocity - 0.2f) < 1e-6);
        CHECK(p->switchDrumming(false, true, error));
        CHECK(!s.drums.enabled && c.drawMode && c.drawNoteCount == 2 && c.drawNotes[0].drumHit);
        CHECK(c.slots[0].oscFreq == 57);
        auto packed = c.drawNotes[0].pack();
        auto unpacked = DrumChannel::DrawNote::unpack(juce::StringArray::fromTokens(packed, ":", ""));
        CHECK(unpacked.drumHit);
        auto legacy = DrumChannel::DrawNote::unpack(
            juce::StringArray::fromTokens("0:1:0:255:0:0:1:0:255:0:1:0:1", ":", ""));
        CHECK(!legacy.drumHit); // historical legato field must never become native-pitch metadata

        CHECK(p->switchDrumming(true, true, error));
        auto state = p->captureStateTree();
        s.drums.assign(15, 80, 2);
        p->applyStateTree(state);
        CHECK(s.drums.patterns[0].count == 2 && s.drums.notes[15] == -1);
        s.drums.clearNotes();
        for (int i = 0; i < 257; ++i)
            s.drums.patterns[0].add({i / 300.0, 0, 1, 0});
        CHECK(!p->switchDrumming(false, true, error));
        CHECK(s.drums.enabled && s.drums.patterns[0].count == 257);
        CHECK(p->switchDrumming(false, false, error));
        CHECK(!s.drums.enabled && c.drawNoteCount == 0);
        // Host MIDI goes to all assigned channels, not the selected one.
        CHECK(p->switchDrumming(true, false, error));
        for (int ch = 0; ch < 8; ++ch)
            s.drums.assign(ch, 60 + ch, 10);
        p->lastSelectedChannel = 15;
        p->prepareToPlay(48000, 512);
        s.drums.arm(0, 0, false);
        juce::AudioBuffer<float> a(2, 512);
        juce::MidiBuffer midi;
        for (int ch = 0; ch < 8; ++ch)
            midi.addEvent(juce::MidiMessage::noteOn(10, 60 + ch, (juce::uint8)(20 + 10 * ch)), 100);
        a.clear();
        p->processBlock(a, midi);
        CHECK(s.drums.patterns[0].count == 8);
        for (int ch = 0; ch < 8; ++ch)
            CHECK(s.drums.patterns[0].hits[(size_t)ch].channel == ch);
        s.drums.stopRecording();
        s.drums.drainLog();
        CHECK(s.drums.takes.size() == 1 && s.drums.takes[0].hits.size() == 8);
        auto file = p->exportMidiFile(15);
        juce::FileInputStream in(file);
        juce::MidiFile mf;
        CHECK(mf.readFrom(in));
        int on = 0;
        for (int i = 0; i < mf.getTrack(0)->getNumEvents(); ++i)
            if (mf.getTrack(0)->getEventPointer(i)->message.isNoteOn())
                ++on;
        CHECK(on == 8);
        file.deleteFile();
        // Note-offs, zero-velocity note-ons and unmatched notes never add drum hits.
        s.stopStandalone();
        s.drums.arm(0, 0, false);
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(10, 60), 0);
        midi.addEvent(juce::MidiMessage::noteOn(10, 60, (juce::uint8)0), 10);
        midi.addEvent(juce::MidiMessage::noteOn(10, 110, (juce::uint8)90), 20);
        a.clear();
        p->processBlock(a, midi);
        CHECK(!s.isCurrentlyPlaying && s.drums.inputCount == 0);
        s.drums.stopRecording();
        s.drums.drainLog();
        // DAW-synced armed pads audition while stopped; only the host starts recording.
        TestPlayHead host;
        p->setPlayHead(&host);
        s.dawSync = true;
        s.drums.arm(0, 0, false);
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(10, 60, (juce::uint8)90), 200);
        a.clear();
        p->processBlock(a, midi);
        CHECK(!s.isCurrentlyPlaying && s.drums.passHead < 0);
        host.playing = true;
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(10, 60, (juce::uint8)90), 200);
        a.clear();
        p->processBlock(a, midi);
        CHECK(s.drums.patterns[0].count == 1);
        CHECK(std::abs(s.drums.patterns[0].hits[0].pos - 200.0 / 96000) < 1.0e-9);
        s.drums.stopRecording();
        s.drums.drainLog();
        p->setPlayHead(nullptr);
        s.dawSync = false;
        s.stopStandalone();
        // MIDI Out uses its independent routing note/channel and the incoming sample offset.
        auto &out = s.patterns[0].channels[15];
        out.midiOut = true;
        out.midiNote = 52;
        out.midiOutChannel = 7;
        s.drums.assign(15, 90, 10);
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(10, 90, (juce::uint8)95), 123);
        a.clear();
        p->processBlock(a, midi);
        int emitted = 0;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn())
            {
                ++emitted;
                CHECK(m.samplePosition == 123 && m.getMessage().getNoteNumber() == 52 &&
                      m.getMessage().getChannel() == 7);
            }
        CHECK(emitted == 1);
        s.drums.arm(0, 0, false);
        p->standaloneStop();
        CHECK(!s.drums.recording && !s.playing);

        out.midiOut = false;
        // Kit takes survive conversion in both directions, splitting only by instrument.
        CHECK(p->switchDrumming(false, true, error));
        CHECK(p->keysTakes.size() == 9);
        CHECK(p->switchDrumming(true, true, error));
        CHECK(s.drums.takes.size() == 9);
        // Middle-bar takes must be visible in the regular group take menu, with their
        // original bar offset. Unmerging before conversion splits them into visible takes.
        s.patterns[1].mergeWithPrev = true;
        LiveDrumming::Take middle;
        middle.name = "Middle bar";
        middle.head = 1;
        middle.bars = 1;
        middle.hits.push_back({1, {0.25, 2, 0.75f, 0}});
        s.drums.takes.push_back(middle);
        CHECK(p->switchDrumming(false, true, error));
        CHECK(p->keysTakes.back().drawPat == 0 && p->keysTakes.back().drawNotes[0].start == 480);
        CHECK(p->switchDrumming(true, true, error));
        CHECK(s.drums.takes.back().head == 0 && s.drums.takes.back().bars == 2);
        CHECK(s.drums.takes.back().hits[0].pattern == 1);
        s.patterns[1].mergeWithPrev = false;
        CHECK(p->switchDrumming(false, true, error));
        CHECK(p->keysTakes.back().drawPat == 1 && p->keysTakes.back().drawNotes[0].start == 96);
        CHECK(p->switchDrumming(true, true, error));
        CHECK(s.drums.takes.size() == 10);
        // Older projects have neither live MIDI routing nor live sequences enabled.
        auto old = p->captureStateTree();
        old.removeChild(old.getChildWithName("LiveDrumming"), nullptr);
        auto live = p->captureStateTree();
        p->applyStateTree(old);
        CHECK(!s.drums.enabled && s.drums.takes.empty());
        p->applyStateTree(live);
        CHECK(s.drums.enabled && s.drums.takes.size() == 10);
        p->releaseResources();
        if (argc > 1 && juce::String(argv[1]) == "--ui")
        {
            CHECK(s.drums.hasNotes());
            s.drums.clearNotes();
            for (int ch = 0; ch < 8; ++ch)
                s.drums.patterns[0].add({0.1 + ch * 0.1, ch, 0.25f + ch * 0.08f, 0});
            std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
            CHECK(s.drums.patterns[0].count == 8);
            ed->setSize(1284, 662);
            CHECK(button(*ed, "LIVE DRUMMING") != nullptr);
            CHECK(button(*ed, "KEYS/RECORD") != nullptr);
            CHECK(button(*ed, "-") != nullptr);
            CHECK(button(*ed, "+") != nullptr);
            auto *keys = keysPanel(*ed);
            CHECK(keys != nullptr);
            if (keys)
            {
                CHECK(keys->comboRecMode.getSelectedId() == 1);
                CHECK(keys->comboRecMode.getText().contains("pad"));
            }
            snapshot(*ed, "basamak-live-drums");
            if (auto *b = button(*ed, "KEYS/RECORD"))
            {
                b->onClick();
                snapshot(*ed, "basamak-live-record");
                CHECK(button(*ed, "REC KIT") != nullptr);
            }
            if (auto *b = button(*ed, "LIVE DRUMMING"))
            {
                b->onClick();
                snapshot(*ed, "basamak-live-conversion");
                CHECK(button(*ed, "Keep and convert") != nullptr);
                auto *cancel = button(*ed, "Cancel");
                CHECK(cancel != nullptr);
                if (cancel)
                    cancel->onClick();
                CHECK(s.drums.enabled);
                b->onClick();
                auto *keep = button(*ed, "Keep and convert");
                if (keep)
                    keep->onClick();
                CHECK(!s.drums.enabled);
                b->onClick();
                if (keep)
                    keep->onClick();
                CHECK(s.drums.enabled);
                if (keys)
                {
                    CHECK(keys->comboRecMode.getSelectedId() == 1);
                    CHECK(keys->comboRecMode.getText().contains("pad"));
                }
                snapshot(*ed, "basamak-live-return");
                auto *editor = dynamic_cast<DrumSequencerEditor *>(ed.get());
                CHECK(editor != nullptr);
                if (editor)
                {
                    editor->keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0));
                    CHECK(!s.drums.enabled);
                    editor->keyPressed(juce::KeyPress(
                        'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0));
                    CHECK(s.drums.enabled);
                }

                const int before = ed->getWidth();
                if (auto *minus = button(*ed, "-"))
                    minus->onClick();
                CHECK(ed->getWidth() < before);
                CHECK(std::abs((double)ed->getWidth() / ed->getHeight() - 1510.0 / 826) < 0.01);
            }
        }
    }
    printf("LiveDrumTest: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
