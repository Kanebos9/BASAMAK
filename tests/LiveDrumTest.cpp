#include "PluginEditor.h"
#include "FactoryContent.h"
#include "MultisampleFixture.h"
#include "GenContext.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <chrono>
#include <ctime>
#include <thread>

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
static juce::ComboBox* firstStepMenu(juce::Component& c)
{
    if (auto* combo = dynamic_cast<juce::ComboBox*>(&c))
        if (combo->getTooltip().startsWith("Number of steps")) return combo;
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        if (auto* combo = firstStepMenu(*c.getChildComponent(i))) return combo;
    return nullptr;
}
static SlotEditor* firstSlotEditor(juce::Component& c, int index = 0)
{
    if (auto* slot = dynamic_cast<SlotEditor*>(&c)) if (slot->index == index) return slot;
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        if (auto* slot = firstSlotEditor(*c.getChildComponent(i), index)) return slot;
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
// Exercise the actual keyboard listener without depending on OS window hit-testing.
struct KeysPanelInputTest
{
    static void down(KeysPanel& panel, int note, float velocity) { panel.handleNoteOn(nullptr, 1, note, velocity); }
    static void up(KeysPanel& panel, int note) { panel.handleNoteOff(nullptr, 1, note, 0); }
};
// Test both input paths through the real processor, including audible voices.
static void monoKeyReturnChecks()
{
    constexpr int blockSize = 128;
    constexpr float blockSeconds = blockSize / 48000.0f;
    for (bool gui : {false, true})
        for (bool poly : {false, true})
            for (bool legato : {false, true})
            {
                const int before = fails;
                auto p = std::make_unique<DrumSequencerProcessor>();
                auto& c = p->sequencer.channel(0);
                tone(c, 261.6256f);
                c.slots[0].atk = 0.2f; c.slots[0].sustain = 0.8f; c.slots[0].release = 0.02f;
                c.keysPolyMode = poly; c.keysLegato = legato;
                c.keysMinVel = 0.2f; c.keysMaxVel = 0.8f;
                p->prepareToPlay(48000, blockSize);
                std::unique_ptr<KeysPanel> panel;
                if (gui) {
                    panel = std::make_unique<KeysPanel>(p->midiLearn);
                    panel->polyMode = poly;
                    panel->onKeyDown = [&](int n, float v) { p->pushKeyDown(n, v); };
                    panel->onKeyUp = [&](int n) { p->pushKeyUp(n); };
                }
                juce::AudioBuffer<float> audio(2, blockSize);
                juce::MidiBuffer midi;
                auto render = [&](int blocks) {
                    for (int b = 0; b < blocks; ++b) { p->processBlock(audio, midi); midi.clear(); }
                };
                auto event = [&](int note, int velocity) {
                    if (gui) {
                        if (velocity) KeysPanelInputTest::down(*panel, note, velocity / 127.0f);
                        else KeysPanelInputTest::up(*panel, note);
                    } else {
                        midi.addEvent(velocity ? juce::MidiMessage::noteOn(3, note, (juce::uint8)velocity)
                                               : juce::MidiMessage::noteOff(3, note), 0);
                    }
                    render(32); // allow the existing 15 ms handover/release to finish
                };
                auto oldestVoiceAge = [&] {
                    float ages[DrumChannel::POLY] = {};
                    const int count = c.activeVoiceTimes(ages, DrumChannel::POLY);
                    CHECK(count >= 1 && (poly || count == 1));
                    return *std::max_element(ages, ages + juce::jmax(1, count));
                };
                event(60, 32); event(71, 111); event(71, 0);
                CHECK(c.keyNoteAudible(60)); CHECK(!c.keyNoteAudible(71));
                CHECK(audio.getMagnitude(0, audio.getNumSamples()) > 0.001f);
                CHECK(p->keysHeldCount == 1 && p->keysHeldNote.load() == 60);
                CHECK(std::abs(p->keysHeldVel.load() - (0.2f + 0.6f * 32.0f / 127)) < 1e-6f);
                // Mono attacks again; Mono Legato continues the connected phrase.
                // Poly's original C continues, without an extra return trigger.
                const float age = oldestVoiceAge();
                CHECK(std::abs(age - (poly || legato ? 96 : 32) * blockSeconds) < 1e-5f);
                event(71, 0); // stale release must not retrigger the returned note
                CHECK(std::abs(oldestVoiceAge() - age - 32 * blockSeconds) < 1e-5f);
                event(60, 0); render(512);
                CHECK(!c.keyNoteAudible(60)); CHECK(p->keysHeldCount == 0);
                CHECK(audio.getMagnitude(0, audio.getNumSamples()) < 1e-5f);

                // Three keys: remove a silent middle key; return to the most recent survivor.
                event(60, 32); event(64, 80); event(67, 111); event(64, 0);
                CHECK(c.keyNoteAudible(67)); CHECK(p->keysHeldCount == 2);
                event(67, 0); CHECK(c.keyNoteAudible(60)); CHECK(!c.keyNoteAudible(64));
                event(60, 0); render(512); CHECK(!c.isPlaying());
                // Releasing the oldest key must never resurrect it on the final release.
                event(60, 32); event(71, 111); event(60, 0);
                CHECK(c.keyNoteAudible(71)); CHECK(p->keysHeldCount == 1);
                event(71, 0); render(512); CHECK(!c.isPlaying());
                printf("[keys] %s %s%s: %s\n", gui ? "keyboard listener" : "MIDI", poly ? "Poly" : "Mono",
                       legato ? " Legato" : "", fails == before ? "PASS" : "FAIL");
            }
    {
        // A split partner can remain Poly even when the selected channel is Mono.
        // Its still-playing C must not gain another voice when B is released.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& first = p->sequencer.channel(0); tone(first, 261.6256f);
        auto& second = p->sequencer.channel(1); tone(second, 261.6256f);
        first.mergeWith = 1; second.mergeWith = 0; first.keysSplitW1 = 60;
        first.keysPolyMode = true; second.keysPolyMode = false;
        first.slots[0].sustain = 0.8f;
        p->lastSelectedChannel = 1; p->prepareToPlay(48000, blockSize);
        juce::AudioBuffer<float> audio(2, blockSize); juce::MidiBuffer midi;
        auto render = [&] { for (int i = 0; i < 32; ++i) { p->processBlock(audio, midi); midi.clear(); } };
        p->pushKeyDown(60, 0.4f); render(); p->pushKeyDown(71, 0.8f); render();
        float ages[DrumChannel::POLY] = {};
        CHECK(first.activeVoiceTimes(ages, DrumChannel::POLY) == 2);
        p->pushKeyUp(71); render();
        CHECK(first.keyNoteAudible(60));
        CHECK(first.activeVoiceTimes(ages, DrumChannel::POLY) == 2);
        CHECK(std::abs(*std::max_element(ages, ages + 2) - 96 * blockSeconds) < 1e-5f);
    }
    {
        // Returning a live note must not inject synthetic note-ons into the recorded roll.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& c = p->sequencer.channel(0); tone(c, 261.6256f);
        c.keysPolyMode = false; c.drawMode = true;
        p->prepareToPlay(48000, blockSize);
        p->keysArmedPattern = 0; p->keysRecording = true;
        juce::AudioBuffer<float> audio(2, blockSize); juce::MidiBuffer midi;
        auto render = [&] { for (int i = 0; i < 32; ++i) { p->processBlock(audio, midi); midi.clear(); } };
        p->pushKeyDown(60, 0.4f); render();
        p->pushKeyDown(71, 0.8f); render();
        p->pushKeyUp(71); render();
        CHECK(c.drawNoteCount == 2);
        CHECK(c.drawNotes[0].semi == 0 && c.drawNotes[1].semi == 11);
        CHECK(c.drawNotes[0].start + c.drawNotes[0].len > c.drawNotes[1].start);
        p->pushKeyUp(60); render(); CHECK(c.drawNoteCount == 2);
    }

}

class EditAudioThread : public juce::Thread
{
public:
    explicit EditAudioThread(std::function<void()> work) : juce::Thread("BASAMAK edit audio test"), runWork(std::move(work)) {}
    void run() override { runWork(); }
private:
    std::function<void()> runWork;
};
int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String(argv[1]) == "--profile-edits")
    {
        auto hasArg = [&](const char* value) { for (int i = 2; i < argc; ++i) if (juce::String(argv[i]) == value) return true; return false; };
        const bool multisample = hasArg("ms");
        const bool live = hasArg("live"), merged = hasArg("merged"), dense = hasArg("dense"), direct = hasArg("direct");
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& song = p->sequencer;
        if (live) Factory::applyPreset(song, Factory::presetNames().indexOf("Live Drums - Room Session"));
        auto& c = song.patterns[0].channels[0];
        tone(c, 220); c.allowOverlap = false;
        if (merged) for (int i = 1; i < 8; ++i) song.patterns[i].mergeWithPrev = true;
        if (dense)
        {
            for (int i = 0; i < 100; ++i)
            {
                LiveDrumming::Take take; take.name = "Kit " + juce::String(i); take.head = i % 64;
                for (int n = 0; n < 256; ++n) take.hits.push_back({take.head, {(double)n / 256, n % 16, 0.8f, 0}});
                song.drums.takes.push_back(std::move(take));
                DrumSequencerProcessor::KeysTake keys; keys.name = "Keys " + juce::String(i); keys.isDraw = true;
                for (int n = 0; n < 256; ++n) { DrumChannel::DrawNote note; note.start = n * 1.5; keys.drawNotes.push_back(note); }
                p->keysTakes.push_back(std::move(keys));
            }
        }
        p->prepareToPlay(48000, 128);
        juce::File msFixture;
        if (multisample) {
            msFixture = makeMultisampleBaseFixture();
            c.slots[0].engine = DrumChannel::SrcSample;
            CHECK(c.loadMultisample(0, msFixture)); c.markDspDirty();
        }
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = *static_cast<DrumSequencerEditor*>(base.get());
        for (int i = 0; i < 9; ++i) ed.timerCallback();
        auto* slot = firstSlotEditor(ed);
        CHECK(slot && slot->freqFader);
        printf("Profile: %s, %s, %s, %s\n", live ? "live" : "regular", merged ? "8-bar merged" : "single bar",
               dense ? "200 saved takes" : "empty takes", direct ? "direct processor" : "JUCE wrapper lock");
        if (multisample) printf("Multisample Base Freq control\n");
        auto start = juce::Time::getMillisecondCounterHiRes();
        auto state = p->captureStateTree();
        printf("Full state capture: %.3f ms\n", juce::Time::getMillisecondCounterHiRes() - start);
        std::atomic<bool> running { true };
        int blocks = 0, silent = 0, overBudget = 0;
        double worstAudioMs = 0, worstLockMs = 0;
        song.startStandalone();
        EditAudioThread audio([&] {
            juce::AudioBuffer<float> buffer(2, 128);
            juce::MidiBuffer midi; midi.ensureSize(1024);
            auto next = std::chrono::steady_clock::now();
            while (running.load())
            {
                midi.clear();
                if (blocks % 12 == 0) midi.addEvent(juce::MidiMessage::noteOn(1, live ? 49 : 60, (juce::uint8)100), 0);
                if (!live && blocks % 12 == 1) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                const double begin = juce::Time::getMillisecondCounterHiRes();
                if (!direct) p->getCallbackLock().enter(); // JUCE VST3/standalone wrapper does this
                const double waited = juce::Time::getMillisecondCounterHiRes() - begin;
                p->processBlock(buffer, midi);
                if (!direct) p->getCallbackLock().exit();
                const double elapsed = juce::Time::getMillisecondCounterHiRes() - begin;
                worstLockMs = juce::jmax(worstLockMs, waited);
                worstAudioMs = juce::jmax(worstAudioMs, elapsed);
                if (elapsed > 128.0 / 48.0) ++overBudget;
                ++blocks;
                if (buffer.getMagnitude(0, 128) < 1.0e-10f) ++silent;
                next += std::chrono::microseconds(2667);
                std::this_thread::sleep_until(next);
            }
        });
        const bool realtime = audio.startRealtimeThread(juce::Thread::RealtimeOptions()
            .withPeriodHz(375).withProcessingTimeMs(0.3).withMaximumProcessingTimeMs(2.6));
        if (!realtime) CHECK(audio.startThread(juce::Thread::Priority::highest));
        printf("Realtime audio scheduling: %s\n", realtime ? "enabled" : "unavailable (priority fallback)");
        double sum = 0, peak = 0, peakCpu = 0, peakEdit = 0;
        for (int edit = 0; edit < 12; ++edit)
        {
            start = juce::Time::getMillisecondCounterHiRes();
            if (slot && slot->freqFader)
                slot->freqFader->setValue(230 + edit * 10, juce::sendNotificationSync); // actual sound-editor callback
            peakEdit = juce::jmax(peakEdit, juce::Time::getMillisecondCounterHiRes() - start);
            for (int tick = 0; tick < 9; ++tick)
            {
                const auto cpuStart = std::clock();
                start = juce::Time::getMillisecondCounterHiRes(); ed.timerCallback();
                double ms = juce::Time::getMillisecondCounterHiRes() - start;
                const double cpuMs = 1000.0 * (std::clock() - cpuStart) / CLOCKS_PER_SEC;
                sum += ms; peak = juce::jmax(peak, ms);
                peakCpu = juce::jmax(peakCpu, cpuMs);
                if (ms >= 16)
                    printf("Slow timer: edit %d tick %d, wall %.3f ms, process CPU %.3f ms (includes audio)\n",
                           edit, tick, ms, cpuMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }
        running = false; CHECK(audio.waitForThreadToExit(5000));
        printf("Sound edits: 12; UI timer mean %.3f ms, worst %.3f ms; audio silent blocks %d / %d\n",
               sum / 108, peak, silent, blocks);
        printf("Audio callback worst %.3f ms; lock wait worst %.3f ms; callbacks over 2.667 ms: %d\n",
               worstAudioMs, worstLockMs, overBudget);
        printf("Timer process CPU worst %.3f ms; sound control callback worst %.3f ms\n", peakCpu, peakEdit);
        CHECK(peak < 50); // catch long UI stalls with ample margin above a normal frame
        CHECK(blocks > 100);
        if (!direct) CHECK(silent == 0);
        if (multisample) msFixture.deleteRecursively();
        return fails ? 1 : 0;
    }
    monoKeyReturnChecks();
    if (argc > 1 && juce::String(argv[1]) == "--mono-keys") return fails ? 1 : 0;
    printf("Live drumming: mapping, timestamps, takes, persistence, conversion and UI\n");
    {
        // A MIDI CC updates every pattern at once. Timer captures may interleave with
        // audio, but must either return a coherent update or retain the previous one.
        auto p = std::make_unique<DrumSequencerProcessor>();
        p->midiLearn.assign("global_masterVol", 7, 1); p->prepareToPlay(48000, 128);
        auto snapshot = p->captureSnapshot();
        const auto initialHash = snapshot.hash();
        std::atomic<bool> running { true };
        std::thread midiThread([&] {
            juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
            for (int n = 0; running.load(); ++n)
            {
                midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 7, 20 + n % 100), 0);
                { const juce::ScopedLock wrapper(p->getCallbackLock()); p->processBlock(audio, midi); }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        bool capturedMidi = false;
        for (int i = 0; i < 12; ++i)
        {
            snapshot = p->captureSnapshot(snapshot, true);
            capturedMidi = capturedMidi || snapshot.hash() != initialHash;
            auto tree = snapshot.toTree();
            const float volume = tree.getChildWithName("Pattern")["mVol"];
            int patterns = 0;
            for (auto pattern : tree) if (pattern.hasType("Pattern"))
            {
                CHECK(std::abs((float)pattern["mVol"] - volume) < 1.0e-6f); ++patterns;
            }
            CHECK(patterns == 64);
        }
        running = false; midiThread.join();
        CHECK(capturedMidi);
    }
    {
        // Incremental undo capture must equal a fresh full capture, including changes
        // outside the selected pattern, both slots, routing, master bus B and saved takes.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& song = p->sequencer;
        auto& ch = song.patterns[63].channels[15];
        auto initial = p->captureSnapshot();
        CHECK(initial.changedChannels() == 64 * 16);
        auto unchanged = p->captureSnapshot(initial);
        CHECK(unchanged.changedChannels() == 0 && unchanged.hash() == initial.hash());
        auto originalTree = initial.toTree();
        ch.slots[1].fmEnvFollow = true; ch.slots[1].modalMorph = 0.37f;
        ch.slots[1].lfoShape[2] = 7; ch.slots[1].lfoCurve[2][63] = 0.625f;
        ch.slots[1].mod[11].curveOn = 1; ch.slots[1].mod[11].curve[63] = 213;
        ch.slots[1].addH[3][31] = 0.375f; ch.slots[1].addPh[3][31] = 0.75f;
        ch.stepVel[63] = 0.321f; ch.stepNudge[63] = -0.375f; ch.steps[63] = true;
        ch.drawMode = true; ch.addDrawNote(0.0123456789, 0.003456789, 5, 200);
        ch.liveChokeBy = 4; ch.chokeGroup = 2; ch.outputBus = 3;
        ch.channelName = "Last lane"; ch.keysMinVel = 0.75f;
        ch.chFxFile[2] = "/snapshot/test-model.nam";
        auto changed = p->captureSnapshot(unchanged);
        CHECK(changed.changedChannels() == 1 && changed.hash() != initial.hash());
        auto changedTree = changed.toTree();
        CHECK(changedTree.isEquivalentTo(p->captureStateTree()));
        CHECK(initial.toTree().isEquivalentTo(originalTree));
        // Caller mutations cannot corrupt either snapshot, or detach shared children.
        changedTree.getChildWithName("Pattern").getChildWithName("Ch").setProperty("volume", 0, nullptr);
        CHECK(changed.toTree().isEquivalentTo(p->captureStateTree()));
        song.patterns[42].master.reverbWetB = 0.456f;
        song.patterns[42].master.tilt = 0.25f;
        song.drums.enabled = true; song.drums.assignAlternate(7, 54);
        song.drums.patterns[63].add({0.123456789012, 15, 0.65f, -0.25f});
        song.drums.takes.push_back({"Saved kit", 63, 1, {{63, {0.12, 15, 0.7f, 0.1f}}}});
        DrumSequencerProcessor::KeysTake take;
        take.name = "Saved roll"; take.channel = 15; take.drawPat = 63; take.isDraw = true;
        take.drawNotes.assign(ch.drawNotes, ch.drawNotes + ch.drawNoteCount); p->keysTakes.push_back(take);
        auto dataEdit = p->captureSnapshot(changed);
        CHECK(dataEdit.changedChannels() == 0 && dataEdit.hash() != changed.hash());
        CHECK(dataEdit.toTree().isEquivalentTo(p->captureStateTree()));
        // Padding is not musical data. Copy construction may leave it different, so
        // unchanged note/take values must still reuse their cached representation.
        static_assert(offsetof(DrumChannel::DrawNote, drumHit) + 1 < sizeof(DrumChannel::DrawNote));
        static_assert(offsetof(LiveDrumming::Hit, pan) + sizeof(float) < sizeof(LiveDrumming::Hit));
        reinterpret_cast<unsigned char*>(&ch.drawNotes[0])[sizeof(DrumChannel::DrawNote) - 1] ^= 0x55;
        reinterpret_cast<unsigned char*>(&p->keysTakes[0].drawNotes[0])[sizeof(DrumChannel::DrawNote) - 1] ^= 0x55;
        reinterpret_cast<unsigned char*>(&song.drums.takes[0].hits[0].hit)[sizeof(LiveDrumming::Hit) - 1] ^= 0x55;
        auto paddingOnly = p->captureSnapshot(dataEdit);
        CHECK(paddingOnly.changedChannels() == 0 && paddingOnly.hash() == dataEdit.hash());
        // View changes ride with the state but do not create an undo step during Follow.
        song.currentPattern = 42; p->visibleChannels = 16; p->editorScale = 0.7;
        auto view = p->captureSnapshot(dataEdit);
        CHECK(view.hash() == dataEdit.hash() && view.changedChannels() == 0);
        CHECK(view.toTree().isEquivalentTo(p->captureStateTree()));
        p->applyStateTree(dataEdit.toTree());
        CHECK(std::abs(ch.slots[1].addH[3][31] - 0.375f) < 1.0e-6f);
        CHECK(ch.slots[1].fmEnvFollow && ch.slots[1].mod[11].curve[63] == 213);
        CHECK(std::abs(ch.stepNudge[63] + 0.375f) < 1.0e-6f);
        CHECK(ch.drawNoteCount == 1 && std::abs(ch.drawNotes[0].len - 0.003456789) < 1.0e-12);
        CHECK(song.drums.takes.size() == 1 && p->keysTakes.size() == 1);
        CHECK(std::abs(song.patterns[42].master.reverbWetB - 0.456f) < 1.0e-6f);
        p->applyStateTree(initial.toTree());
        CHECK(!song.drums.enabled && song.drums.takes.empty() && p->keysTakes.empty());
        CHECK(ch.drawNoteCount == 0 && !ch.slots[1].fmEnvFollow);
        CHECK(ch.stepNudge[63] == 0);
    }
    if (argc > 1 && juce::String(argv[1]) == "--ui")
    {
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& ch = p->sequencer.patterns[0].channels[0];
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = *static_cast<DrumSequencerEditor*>(base.get());
        auto* undo = button(ed, "Undo"); auto* redo = button(ed, "Redo");
        CHECK(undo && redo);
        auto settle = [&] { for (int i = 0; i < 9; ++i) ed.timerCallback(); };
        settle();
        if (auto* keyboardPanel = keysPanel(ed)) {
            CHECK(!keyboardPanel->arpOrLetRing);
            keyboardPanel->arpEditor.on = true; keyboardPanel->arpEditor.onChange();
            CHECK(keyboardPanel->arpOrLetRing); // update immediately, before the next key/timer refresh
            keyboardPanel->arpEditor.on = false; keyboardPanel->arpEditor.onChange();
            CHECK(!keyboardPanel->arpOrLetRing);
        } else CHECK(false);
        const float original = ch.slots[0].oscFreq;
        ch.slots[0].oscFreq = 333; ch.markDspDirty(); settle();
        ch.slots[0].oscFreq = 444; ch.markDspDirty();
        // The timer observes this edit but it has not settled into an undo entry yet.
        for (int i = 0; i < 3; ++i) ed.timerCallback();
        if (undo && redo)
        {
            undo->onClick(); CHECK(std::abs(ch.slots[0].oscFreq - 333) < 0.001f);
            undo->onClick(); CHECK(std::abs(ch.slots[0].oscFreq - original) < 0.001f);
            redo->onClick(); CHECK(std::abs(ch.slots[0].oscFreq - 333) < 0.001f);
            redo->onClick(); CHECK(std::abs(ch.slots[0].oscFreq - 444) < 0.001f);
            undo->onClick(); ch.slots[0].oscFreq = 555; ch.markDspDirty(); settle();
            CHECK(!redo->isEnabled());
            undo->onClick(); CHECK(std::abs(ch.slots[0].oscFreq - 333) < 0.001f);
            // An eight-bar merged edit is one undo action and restores every member.
            for (int i = 1; i < 8; ++i) p->sequencer.patterns[i].mergeWithPrev = true;
            settle(); ch.slots[0].oscFreq = 666; ch.markDspDirty(); settle();
            for (int i = 0; i < 8; ++i) CHECK(std::abs(p->sequencer.patterns[i].channels[0].slots[0].oscFreq - 666) < 0.001f);
            undo->onClick();
            for (int i = 0; i < 8; ++i) CHECK(std::abs(p->sequencer.patterns[i].channels[0].slots[0].oscFreq - 333) < 0.001f);
        }
    }
    {
        const auto fixture = makeMultisampleBaseFixture();
        auto p = std::make_unique<DrumSequencerProcessor>();
        p->prepareToPlay(48000, 128);
        auto& c = p->sequencer.channel(0);
        tone(c, 220); c.drawMode = false; c.clearStepData();
        c.slots[0].engine = DrumChannel::SrcSample;
        CHECK(c.loadMultisample(0, fixture));
        c.slots[0].msBaseFreq = 130.8127827f;
        CHECK(GenContext::stepChannelBaseMidi(c) == 48);
        // Keyboard recording stays absolute on input and stores the offset from the base.
        p->lastSelectedChannel = 0; p->keysRecMode = 0; p->keysArmedPattern = 0; p->keysRecording = true;
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        p->processBlock(audio, midi);
        CHECK(c.steps[0] && std::abs(c.stepPitch[0] - 12.0f) < 0.001f);
        p->keysRecording = false; p->sequencer.stopStandalone();
        c.clearStepData(); c.numSteps = 8; c.steps[0] = true; c.stepPitch[0] = 2;
        auto saved = p->captureStateTree();
        c.slots[0].msBaseFreq = 261.6255653f; p->applyStateTree(saved);
        CHECK(std::abs(c.slots[0].msBaseFreq - 130.8127827f) < 0.001f);
        auto exportNotes = [&] {
            auto f = p->exportMidiFile(0); juce::FileInputStream in(f); juce::MidiFile mf;
            CHECK(mf.readFrom(in)); std::vector<int> notes;
            if (auto* t = mf.getTrack(0)) for (int i=0; i<t->getNumEvents(); ++i)
                if (t->getEventPointer(i)->message.isNoteOn()) notes.push_back(t->getEventPointer(i)->message.getNoteNumber());
            f.deleteFile(); return notes;
        };
        CHECK(exportNotes() == std::vector<int>{50}); // D3, relative to C3 base
        c.slots[1] = c.slots[0]; c.slots[1].msBaseFreq = 523.2511306f;
        CHECK(c.loadMultisample(1, fixture));
        CHECK(exportNotes() == (std::vector<int>{50, 74})); // independent bases in the two slots
        if (argc > 1 && juce::String(argv[1]) == "--ui") {
            DrumSequencerEditor ed(*p); ed.setVisible(true);
            for (int i=0; i<9; ++i) ed.timerCallback(); // capture the editor's initial undo baseline
            auto* slot0 = firstSlotEditor(ed); auto* slot1 = firstSlotEditor(ed, 1);
            CHECK(slot0 && slot1 && slot0->msFace && slot1->msFace);
            if (slot0 && slot1) {
                CHECK(slot0->freqFader->isVisible() && slot0->freqFader->isEnabled());
                CHECK(slot0->getLocalBounds().contains(slot0->freqFader->getBounds()));
                CHECK(slot1->getLocalBounds().contains(slot1->freqFader->getBounds()));
                CHECK(slot0->freqFader->getTooltip().contains("Base Freq"));
                slot0->freqFader->setValue(65.4063913, juce::sendNotificationSync);
                CHECK(std::abs(c.slots[0].msBaseFreq - 65.4063913) < 0.001);
                CHECK(std::abs(c.slots[1].msBaseFreq - 523.2511306) < 0.001);
                for (int i=0; i<9; ++i) ed.timerCallback();
                auto* undo = button(ed, "Undo"); CHECK(undo);
                if (undo) undo->onClick();
                CHECK(std::abs(c.slots[0].msBaseFreq - 130.8127827) < 0.001);
                snapshot(ed, "basamak-multisample-base-frequency");
                // The same base survives the real slot-copy action.
                slot1->onCopyFromSlot(0);
                CHECK(c.slots[1].msBaseFreq == c.slots[0].msBaseFreq);
                // The visible frequency control's learned MIDI CC updates the same slot and readout.
                const float previousOscHz = c.slots[0].oscFreq;
                p->midiLearn.assign("ui_sel_slotFreq", 23, 1);
                midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 23, 50), 0);
                p->processBlock(audio, midi); ed.timerCallback();
                const double ccHz = 20.0 * std::pow(4186.0 / 20.0, 50.0f / 127.0f);
                CHECK(std::abs(c.slots[0].msBaseFreq - ccHz) < 0.001);
                CHECK(std::abs(slot0->freqFader->getValue() - ccHz) < 0.001);
                CHECK(c.slots[0].oscFreq == previousOscHz);
                CHECK(std::abs(c.slots[1].msBaseFreq - 130.8127827) < 0.001);
                // A merged slot edit preserves already-playing multisample voices
                // and their buffers instead of reloading the instrument per knob tick.
                p->sequencer.patterns[1].mergeWithPrev = true;
                auto settle = [&] { for (int i=0; i<9; ++i) ed.timerCallback(); };
                settle();
                auto& next = p->sequencer.patterns[1].channels[0];
                next.steps[3] = true;
                const auto* samples = next.slotSample[0].buf.getReadPointer(0);
                const auto zones = next.msSet[0];
                next.trigger(1, 0); CHECK(next.anyVoiceActive());
                slot0->freqFader->setValue(220, juce::sendNotificationSync);
                c.slots[0].filterCutoff = 1500; c.markDspDirty(); settle();
                CHECK(next.anyVoiceActive());
                CHECK(next.slotSample[0].buf.getReadPointer(0) == samples && next.msSet[0] == zones);
                CHECK(std::abs(next.slots[0].msBaseFreq - 220) < 0.001f);
                CHECK(next.slots[0].filterCutoff == 1500 && next.steps[3]);
                // Channel settings and engine/waveform changes still use the full
                // load/bake path, including the granular source's actual table.
                c.volume = 0.3f; settle(); CHECK(next.volume == c.volume);
                c.slots[0].engine = DrumChannel::SrcGrain;
                c.slots[0].oscShape = DrumChannel::WvSaw;
                c.rebuildAddTables(); c.markDspDirty(); settle();
                CHECK(!c.grainTbl[0].empty() && next.grainTbl[0] == c.grainTbl[0]);
                c.slots[0].oscShape = DrumChannel::WvSquare;
                c.rebuildAddTables(); c.markDspDirty(); settle();
                CHECK(next.grainTbl[0] == c.grainTbl[0]);
            }
        }
        fixture.deleteRecursively();
    }
    {
        // New step choices survive menu selection, persistence and MIDI export. Each
        // bar keeps its own duration even when a merged pair has different counts.
        const int counts[] = {17, 18, 19, 25, 26, 33, 34, 35};
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& sq = p->sequencer;
        if (argc > 1 && juce::String(argv[1]) == "--ui") {
            DrumSequencerEditor ed(*p); ed.setVisible(true);
            auto* combo = firstStepMenu(ed); CHECK(combo);
            if (combo) {
                auto duration = [&](int id) {
                    juce::PopupMenu::MenuItemIterator it(*combo->getRootMenu(), true);
                    while (it.next()) if (it.getItem().itemID == id) return it.getItem().shortcutKeyDescription;
                    return juce::String();
                };
                auto refresh = [&] { for (int tick=0; tick<3; ++tick) ed.timerCallback(); };
                refresh(); CHECK(duration(4) == "0.5 s/step");
                CHECK(duration(35) == "0.057 s/step");
                sq.standaloneBpm = 60; refresh(); CHECK(duration(4) == "1 s/step");
                sq.timeSigNum = 3; refresh(); CHECK(duration(4) == "0.75 s/step");
                sq.dawSync = true; p->currentBpm = 120; p->currentTimeSigNum = 7; p->currentTimeSigDen = 8;
                refresh(); CHECK(duration(4) == "0.438 s/step");
                sq.dawSync = false; sq.standaloneBpm = 120; sq.timeSigNum = 4;
                p->currentTimeSigNum = p->currentTimeSigDen = 4; refresh();
                for (int n : counts) {
                    CHECK(combo->indexOfItemId(n) >= 0);
                    combo->setSelectedId(n, juce::sendNotificationSync);
                    CHECK(sq.channel(0).numSteps == n && !sq.channel(0).drawMode);
                    ed.timerCallback(); CHECK(combo->getSelectedId() == n);
                }
                sq.patterns[1].mergeWithPrev = true;
                sq.patterns[1].channels[0].numSteps = 17;
                ed.timerCallback();
                // Per-bar 35 + 17 fits; all-bars 35 + 35 must remain rejected.
                combo->setSelectedId(2033, juce::sendNotificationSync);
                CHECK(sq.channel(0).numSteps == 33 && sq.patterns[1].channels[0].numSteps == 17);
                refresh(); CHECK(duration(2033) == "0.061 s/step");
                combo->setSelectedId(35, juce::sendNotificationSync);
                CHECK(sq.channel(0).numSteps == 33 && sq.patterns[1].channels[0].numSteps == 17);
                combo->setSelectedId(26, juce::sendNotificationSync);
                CHECK(sq.channel(0).numSteps == 26 && sq.patterns[1].channels[0].numSteps == 26);
            }
        }
        sq.patterns[1].mergeWithPrev = true;
        for (int b = 0; b < 2; ++b) for (int ch = 0; ch < 8; ++ch) {
            auto& c = sq.patterns[b].channels[ch];
            c.clearStepData(); c.drawMode = false;
            c.numSteps = b == 0 ? counts[ch] : 17;
            c.midiOut = true; c.midiOutChannel = ch + 1; c.midiNote = 36 + ch;
            for (int st = 0; st < c.numSteps; ++st) c.steps[st] = true;
        }
        auto saved = p->captureStateTree();
        for (int ch = 0; ch < 8; ++ch) sq.patterns[0].channels[ch].numSteps = 8;
        p->applyStateTree(saved);
        for (int ch = 0; ch < 8; ++ch) {
            CHECK(sq.patterns[0].channels[ch].numSteps == counts[ch]);
            CHECK(sq.patterns[1].channels[ch].numSteps == 17);
            auto file = p->exportMidiFile(ch);
            juce::FileInputStream stream(file); juce::MidiFile mf;
            CHECK(mf.readFrom(stream)); CHECK(mf.getTimeFormat() == 9600);
            int ons = 0;
            if (auto* track = mf.getTrack(0)) {
                for (int i = 0; i < track->getNumEvents(); ++i) {
                    const auto& msg = track->getEventPointer(i)->message;
                    if (!msg.isNoteOn()) continue;
                    const bool second = ons >= counts[ch];
                    const int step = second ? ons - counts[ch] : ons;
                    const double expected = (second ? 38400.0 : 0.0)
                        + step * 38400.0 / (second ? 17 : counts[ch]);
                    CHECK(std::abs(msg.getTimeStamp() - expected) <= 1.0);
                    CHECK(msg.getChannel() == ch + 1);
                    ++ons;
                }
                CHECK(track->getEndTime() == 76800);
            } else CHECK(false);
            CHECK(ons == counts[ch] + 17); file.deleteFile();
        }
    }
    {
        // Fractional roll timing: sample-offset MIDI, short gates, close hits, persistence,
        // backward compatibility and both exports. No old 384-column quantization remains.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto &s = p->sequencer;
        auto &c = s.patterns[0].channels[0];
        tone(c, 261.625565f);
        c.drawMode = true;
        p->prepareToPlay(48000, 512);
        p->keysArmedPattern = 0;
        s.startStandalone();
        juce::AudioBuffer<float> a(2, 512);
        juce::MidiBuffer midi;
        p->processBlock(a, midi); // start at sample 512
        p->keysRecording = true;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)95), 33);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 211);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)73), 289);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 450);
        p->processBlock(a, midi);
        p->keysRecording = false;
        CHECK(c.drawNoteCount == 2);
        if (c.drawNoteCount == 2)
        {
            CHECK(std::abs(c.drawNotes[0].start - 545.0 / 250) < 1e-9);
            CHECK(std::abs(c.drawNotes[0].len - 178.0 / 250) < 1e-9);
            CHECK(std::abs(c.drawNotes[1].start - 801.0 / 250) < 1e-9);
            CHECK(std::abs(c.drawNotes[1].len - 161.0 / 250) < 1e-9);
        }
        DrumSequencerProcessor::KeysTake take;
        take.name = "Fine timing"; take.channel = 0; take.drawPat = 0; take.isDraw = true;
        take.drawNotes.assign(c.drawNotes, c.drawNotes + c.drawNoteCount);
        p->keysTakes.push_back(take);
        auto state = p->captureStateTree();
        c.clearDrawNotes();
        p->applyStateTree(state);
        CHECK(c.drawNoteCount == 2 && std::abs(c.drawNotes[0].start - 2.18) < 1e-9);
        CHECK(p->keysTakes.size() == 1 && std::abs(p->keysTakes[0].drawNotes[0].len - 0.712) < 1e-9);
        auto old = DrumChannel::DrawNote::unpack(juce::StringArray::fromTokens("96:48:7:200", ":", ""));
        CHECK(old.start == 96 && old.len == 48 && old.semi == 7);
        // Both onsets lie close to a grid line; render must keep their exact positions/gates.
        s.reset(); s.startStandalone(); s.recordSuppressCh = -1;
        int count = 0;
        for (int block = 0; block < 3; ++block)
            for (const auto &e : s.processBlock(a, 48000, 512, nullptr))
                if (e.isDraw && e.channel == 0)
                {
                    const int expected = count == 0 ? 545 : 801;
                    CHECK(std::abs(block * 512 + e.offset - expected) <= 1);
                    CHECK(std::abs(e.gate - (count == 0 ? 178 : 161)) <= 1);
                    ++count;
                }
        CHECK(count == 2);
        auto verifyExport = [&](bool kit)
        {
            auto file = p->exportMidiFile(kit ? 15 : 0);
            juce::FileInputStream stream(file);
            juce::MidiFile mf;
            CHECK(mf.readFrom(stream));
            CHECK(mf.getTimeFormat() == 9600);
            int ons = 0;
            for (int i = 0; i < mf.getTrack(0)->getNumEvents(); ++i)
            {
                auto &msg = mf.getTrack(0)->getEventPointer(i)->message;
                if (msg.isNoteOn())
                {
                    CHECK(msg.getNoteNumber() == (kit ? 49 : 60));
                    CHECK(std::abs(msg.getTimeStamp() - (ons == 0 ? 218.0 : 320.4)) <= 1.0);
                    CHECK(msg.getVelocity() == (ons == 0 ? 95 : 73));
                    ++ons;
                }
            }
            CHECK(ons == 2);
            CHECK(mf.getTrack(0)->getEndTime() == 38400); // silent remainder survives dragging
            file.deleteFile();
        };
        verifyExport(false);
        juce::String error;
        CHECK(p->switchDrumming(true, true, error));
        CHECK(std::abs(s.drums.patterns[0].hits[0].pos * 384 - 2.18) < 1e-9);
        verifyExport(true);
        CHECK(p->switchDrumming(false, true, error));
        CHECK(std::abs(c.drawNotes[0].start - 2.18) < 1e-9);
        // Host start at sample zero must be capturable too (including a sub-column release).
        c.clearDrawNotes(); s.reset(); s.dawSync = true;
        TestPlayHead host; host.playing = true; p->setPlayHead(&host);
        p->keysRecording = true; p->keysLoopSeen = -1;
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8)100), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 62), 127);
        p->processBlock(a, midi);
        CHECK(c.drawNoteCount == 1);
        CHECK(c.drawNotes[0].start == 0 && std::abs(c.drawNotes[0].len - 127.0 / 250) < 1e-9);
        p->keysRecording = false; p->setPlayHead(nullptr);
        c.clearDrawNotes(); s.dawSync = false; s.reset(); s.startStandalone();
        s.recordSuppressCh = -1;
        c.addDrawNote(2.01, 0.03, 0, 100);
        c.addDrawNote(2.1, 0.03, 2, 100); // same old integer column, across an audio-block boundary
        int closeHits = 0;
        for (int block = 0; block < 2; ++block)
            for (const auto &e : s.processBlock(a, 48000, 512, nullptr))
                if (e.channel == 0 && e.isDraw) ++closeHits;
        CHECK(closeHits == 2);
    }
    {
        // A host bar boundary inside a callback closes the previous take precisely,
        // then keeps the held key in the next pass until its sample-offset release.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& s = p->sequencer;
        auto& c = s.patterns[0].channels[0];
        tone(c, 261.625565f); c.drawMode = true;
        p->prepareToPlay(48000, 512);
        TestPlayHead host; host.playing = true; host.ppq = 95990.0 / 24000;
        p->setPlayHead(&host); s.dawSync = true;
        p->keysArmedPattern = 0; p->keysRecording = true;
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 5);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 30);
        p->processBlock(audio, midi);
        CHECK(p->keysDrawTakeReady.load() && p->keysDrawTakeCount == 1);
        CHECK(std::abs(p->keysDrawTakeNotes[0].start - 383.98) < 1e-9);
        CHECK(std::abs(p->keysDrawTakeNotes[0].len - 0.02) < 1e-9);
        CHECK(c.drawNoteCount == 1 && c.drawNotes[0].start == 0);
        CHECK(std::abs(c.drawNotes[0].len - 0.08) < 1e-9);
        p->keysRecording = false; p->setPlayHead(nullptr);
    }
    {
        auto p = std::make_unique<DrumSequencerProcessor>();
        CHECK(!p->hasDrummingSwitchData());
        auto &last = p->sequencer.patterns[63].channels[15];
        last.steps[63] = true; // even data hidden by the current step count must be protected
        CHECK(p->hasDrummingSwitchData()); last.steps[63] = false;
        last.addDrawNote(0.123, 0.456, 0, 100);
        CHECK(p->hasDrummingSwitchData()); last.clearDrawNotes();
        p->sequencer.drums.takes.push_back({"Saved kit take", 63, 1, {{63, {0.1, 15, 1, 0}}}});
        CHECK(p->hasDrummingSwitchData()); p->sequencer.drums.takes.clear();
        if (argc > 1 && juce::String(argv[1]) == "--ui")
        {
            auto &c = p->sequencer.patterns[0].channels[0];
            c.drawMode = true;
            StepGridComponent grid;
            grid.setSize(1000, 352); grid.setGridDiv(0);
            grid.update(p->sequencer, false);
            std::vector<DrumChannel::DrawNote> edited;
            grid.onDrawNotesChanged = [&](int, const DrumChannel::DrawNote* notes, int count)
            { edited.assign(notes, notes + count); };
            auto mouse = [&](float x, bool dragged)
            {
                auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {x, 22},
                    juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                    &grid, &grid, now, {103, 22}, now, 1, dragged);
            };
            grid.mouseDown(mouse(103, false));
            grid.mouseDrag(mouse(119, true));
            grid.mouseUp(mouse(119, true));
            CHECK(edited.size() == 1);
            if (!edited.empty()) CHECK(std::abs(edited[0].start - 103.0 / 1000 * 384) < 1e-9);
            std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
            auto* toggle = button(*ed, "LIVE DRUMMING");
            auto* keep = button(*ed, "Keep and convert");
            CHECK(toggle && keep);
            if (toggle && keep)
            {
                toggle->onClick();
                CHECK(p->sequencer.drums.enabled && !keep->getParentComponent()->isVisible());
                toggle->onClick();
                CHECK(!p->sequencer.drums.enabled && !keep->getParentComponent()->isVisible());
                last.steps[3] = true;
                toggle->onClick();
                CHECK(!p->sequencer.drums.enabled && keep->getParentComponent()->isVisible());
                button(*ed, "Cancel")->onClick();
                CHECK(last.steps[3]);
            }
        }
    }
    {
        auto d = std::make_unique<LiveDrumming>();
        const int padNotes[] = {49, 48, 45, 51, 36, 38, 43, 42};
        for (int ch = 0; ch < 8; ++ch) CHECK(d->target(padNotes[ch], 10) == ch);
        CHECK(d->target(127, 10) == -1);
        CHECK(d->target(42, 10) == 7 && d->target(46, 10) == 7);
        CHECK(d->target(-1, 10) == -1);
        d->assign(12, 46, 10); // claiming an alternate removes it from the old row
        CHECK(d->target(46, 10) == 12 && d->alternateNotes[7] == -1);
        d->assignAlternate(7, 46);
        CHECK(d->notes[12] == -1 && d->target(46, 10) == 7);
        d->assign(12, 42, 10); // if primary moves, remaining alternate becomes the primary
        CHECK(d->notes[7] == 46 && d->alternateNotes[7] == -1);
        d->defaultMap();
        auto aliases = d->save();
        d->restore(aliases);
        CHECK(d->target(42, 10) == 7 && d->target(46, 10) == 7);
        for (auto child : aliases) if (child.hasType("Map"))
        {
            child.removeProperty("alternate", nullptr);
            if ((int)child.getProperty("ch") == 8) child.setProperty("note", 46, nullptr);
        }
        d->restore(aliases); // legacy custom maps are unchanged, no implicit aliases
        CHECK(d->target(42, 10) == 7 && d->target(46, 10) == 8);
        d->defaultMap();
        d->learnChannel = 7;
        d->noteOn(70, 4, 95, 0);
        CHECK(d->notes[7] == 70 && d->alternateNotes[7] == -1 && d->target(46, 10) == -1);
        d->assignAlternate(7, 71);
        d->assign(12, 71, 5); // same note, different MIDI channel, still unambiguous
        CHECK(d->target(71, 4) == 7 && d->target(71, 5) == 12);
        d->assign(7, 70, 0); // widening to Any claims BOTH inputs
        CHECK(d->target(71, 5) == 7 && d->notes[12] == -1);
        d->assign(7, -1, 0);
        CHECK(d->target(70, 4) == -1 && d->target(71, 5) == -1);
        d->defaultMap();
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
        CHECK(!d->enabled && d->takes.empty() && d->notes[0] == 49);
    }
    {
        // Velocity range is applied once at input: stopped monitoring, recording and replay
        // must agree, including very quiet MIDI and fixed full-velocity pads.
        auto s = std::make_unique<Sequencer>();
        s->drums.enabled = true;
        s->drums.assign(0, 60, 10);
        s->drums.assign(1, 61, 10);
        for (int ch = 0; ch < 2; ++ch)
        {
            tone(s->patterns[0].channels[ch], 160.0f + ch * 110);
            s->patterns[0].channels[ch].prepareToPlay(48000, 512);
        }
        auto& c = s->patterns[0].channels[0];
        juce::AudioBuffer<float> b(2, 512);
        s->drums.noteOn(60, 10, 1, 23);
        b.clear(); auto events = s->processBlock(b, 48000, 512, nullptr);
        CHECK(events.size() == 1 && std::abs(events[0].drawVel - 1.0f / 127) < 1e-8f);
        c.keysMinVel = 0.25f; c.keysMaxVel = 0.75f;
        s->patterns[0].channels[1].keysMinVel = 1;
        s->reset(); s->drums.beginBlock();
        for (int ch = 0; ch < 2; ++ch) s->patterns[0].channels[ch].prepareToPlay(48000, 512);
        s->drums.noteOn(60, 10, 20, 111);
        s->drums.noteOn(61, 10, 1, 222);
        b.clear(); events = s->processBlock(b, 48000, 512, nullptr);
        const float adjusted = 0.25f + (20.0f / 127) * 0.5f;
        CHECK(events.size() == 2 && std::abs(events[0].drawVel - adjusted) < 1e-7f && events[1].drawVel == 1);
        juce::AudioBuffer<float> audition; audition.makeCopyOf(b);
        s->reset(); s->drums.arm(0, 0, false); s->startStandalone();
        for (int ch = 0; ch < 2; ++ch) s->patterns[0].channels[ch].prepareToPlay(48000, 512);
        s->drums.beginBlock(); s->drums.noteOn(60, 10, 20, 111); s->drums.noteOn(61, 10, 1, 222);
        b.clear(); s->processBlock(b, 48000, 512, nullptr);
        float delta = 0;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i)
            delta = std::max(delta, std::abs(b.getSample(ch, i) - audition.getSample(ch, i)));
        printf("[velocity] stopped vs recording maximum difference %.9f\n", delta);
        CHECK(delta < 1e-6f);
        s->drums.stopRecording(); s->drums.drainLog();
        CHECK(s->drums.takes.size() == 1 && s->drums.takes[0].hits.size() == 2);
        s->stopStandalone(); s->reset(); c.keysMinVel = c.keysMaxVel = 1;
        s->startStandalone(); s->drums.beginBlock();
        for (int ch = 0; ch < 2; ++ch) s->patterns[0].channels[ch].prepareToPlay(48000, 512);
        b.clear(); events = s->processBlock(b, 48000, 512, nullptr);
        CHECK(events.size() == 2 && std::abs(events[0].drawVel - adjusted) < 1e-7f);
        delta = 0;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i)
            delta = std::max(delta, std::abs(b.getSample(ch, i) - audition.getSample(ch, i)));
        printf("[velocity] stopped vs playback maximum difference %.9f\n", delta);
        CHECK(delta < 1e-6f); // changing range afterwards must not rescale recorded hits
    }
    {
        // Slot Offset delays only slot 2, by exactly 5 ms on an actual live MIDI hit.
        auto s = std::make_unique<Sequencer>();
        s->drums.enabled = true; s->drums.assign(0, 60, 10);
        auto& c = s->patterns[0].channels[0];
        tone(c, 220); c.slots[0].weight = 0.5f; c.slots[0].pan = -1;
        c.slots[1] = c.slots[0]; c.slots[1].pan = 1; c.slots[1].oscFreq = 330;
        c.humanizeAmt = 0.05f;
        c.prepareToPlay(48000, 512);
        juce::AudioBuffer<float> b(2, 512);
        s->drums.noteOn(60, 10, 100, 100);
        b.clear(); s->processBlock(b, 48000, 512, nullptr);
        CHECK(b.getMagnitude(0, 101, 239) > 0.001f);
        CHECK(b.getMagnitude(1, 0, 340) < 1e-7f);
        CHECK(b.getMagnitude(1, 341, 171) > 0.001f);
        c.humanizeAmt = 0; s->reset(); s->drums.beginBlock();
        s->drums.noteOn(60, 10, 100, 100);
        b.clear(); s->processBlock(b, 48000, 512, nullptr);
        CHECK(b.getMagnitude(1, 101, 239) > 0.001f);
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
        s.patterns[0].channels[0].keysMinVel = 0.25f;
        s.patterns[0].channels[0].keysMaxVel = 0.75f;
        s.patterns[0].channels[1].keysMinVel = s.patterns[0].channels[1].keysMaxVel = 1.0f;
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
        for (const auto& hit : s.drums.takes[0].hits)
        {
            const float expected = hit.hit.channel == 0 ? 0.25f + (20.0f / 127) * 0.5f
                                 : hit.hit.channel == 1 ? 1.0f : (20.0f + 10 * hit.hit.channel) / 127;
            CHECK(std::abs(hit.hit.velocity - expected) < 1e-7f);
        }
        auto file = p->exportMidiFile(15);
        juce::FileInputStream in(file);
        juce::MidiFile mf;
        CHECK(mf.readFrom(in));
        int on = 0;
        for (int i = 0; i < mf.getTrack(0)->getNumEvents(); ++i)
            if (const auto& message = mf.getTrack(0)->getEventPointer(i)->message; message.isNoteOn())
            {
                ++on;
                if (message.getNoteNumber() == 60) CHECK(message.getVelocity() == 42);
                if (message.getNoteNumber() == 61) CHECK(message.getVelocity() == 127);
            }
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
                CHECK(keys->minVelKnob.isEnabled() && keys->maxVelKnob.isEnabled());
                CHECK(!keys->strumKnob.isEnabled() && !keys->glideKnob.isEnabled());
                keys->minVelKnob.setValue(0.8, juce::sendNotificationSync);
                keys->maxVelKnob.setValue(0.6, juce::sendNotificationSync);
                CHECK(s.channel(p->lastSelectedChannel).keysMinVel == 0.6f);
                CHECK(s.channel(p->lastSelectedChannel).keysMaxVel == 0.6f);
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
    {
        // The normal physical OKTO pads keep their instruments even with another editor row selected.
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& s = p->sequencer;
        const int kit = Factory::presetNames().indexOf("Live Drums - Room Session");
        CHECK(kit >= 0);
        Factory::applyPreset(s, kit);
        CHECK(s.drums.enabled);
        CHECK(s.patterns[0].channels[4].mixName == "Snap Kick");
        CHECK(s.patterns[0].channels[5].mixName == "Mod Snare");
        CHECK(s.patterns[0].channels[2].liveChokeBy == 4);
        juce::MemoryBlock savedKit; p->getStateInformation(savedKit);
        p->setStateInformation(savedKit.getData(), (int)savedKit.getSize());
        CHECK(s.patterns[63].channels[2].liveChokeBy == 4);
        CHECK(s.drums.target(46, 10) == 7 && s.drums.target(42, 10) == 7);
        CHECK(s.patterns[63].channels[4].keysMinVel == 1);
        p->lastSelectedChannel = 15;
        p->prepareToPlay(48000, 512);
        s.drums.arm(0, 0, false);
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        const int physicalNotes[] = {49, 48, 45, 51, 36, 38, 43, 46};
        for (int ch = 0; ch < 8; ++ch)
            midi.addEvent(juce::MidiMessage::noteOn(10, physicalNotes[ch], (juce::uint8)100), 10 + ch * 50);
        p->processBlock(audio, midi);
        CHECK(s.drums.patterns[0].count == 8);
        for (int ch = 0; ch < s.drums.patterns[0].count; ++ch)
            CHECK(s.drums.patterns[0].hits[(size_t)ch].channel == ch);
        CHECK(audio.getMagnitude(0, 512) > 0.001f);
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(10, 42, (juce::uint8)40), 77);
        p->processBlock(audio, midi);
        CHECK(s.drums.patterns[0].count == 9 && s.drums.patterns[0].hits[8].channel == 7);
        auto exported = p->exportMidiFile(15);
        juce::FileInputStream stream(exported); juce::MidiFile kitMidi;
        CHECK(kitMidi.readFrom(stream));
        int hats = 0, extra = 0;
        for (int i = 0; i < kitMidi.getTrack(0)->getNumEvents(); ++i)
        {
            const auto& message = kitMidi.getTrack(0)->getEventPointer(i)->message;
            if (message.isNoteOn()) { if (message.getNoteNumber() == 42) ++hats; if (message.getNoteNumber() == 46) ++extra; }
        }
        CHECK(hats == 2 && extra == 0);
        exported.deleteFile();
        p->standaloneStop();
        p->releaseResources();
        if (argc > 1 && juce::String(argv[1]) == "--ui")
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
            auto* overlap = button(*ed, "OV");
            CHECK(overlap && overlap->isEnabled());
            if (overlap)
            {
                const bool before = s.patterns[0].channels[0].allowOverlap;
                overlap->setToggleState(!before, juce::dontSendNotification);
                overlap->onClick();
                CHECK(s.patterns[0].channels[0].allowOverlap == !before);
                overlap->setToggleState(before, juce::dontSendNotification);
                overlap->onClick();
                CHECK(s.patterns[0].channels[0].allowOverlap == before);
            }
            snapshot(*ed, "basamak-okto-physical-kit");
        }
    }
    if (argc > 1 && juce::String(argv[1]) == "--ui")
    {
        auto p = std::make_unique<DrumSequencerProcessor>();
        auto& s = p->sequencer; s.drums.enabled = true;
        auto& c = s.channel(0); tone(c, 220); c.slots[1] = c.slots[0];
        c.keysMinVel = 0.1f; c.keysMaxVel = 0.9f; c.humanizeAmt = 0.25f;
        p->prepareToPlay(48000, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
        auto* keys = keysPanel(*ed); CHECK(keys != nullptr);
        if (keys)
        {
            CHECK(keys->humanKnob.isEnabled());
            CHECK(std::abs(keys->humanKnob.getValue() - 0.25) < 1e-6);
            keys->humanKnob.setValue(0.5, juce::sendNotificationSync);
            CHECK(c.humanizeAmt == 0.5f);
            p->midiLearn.assign("ui_sel_minVel", 20, 1);
            p->midiLearn.assign("ui_sel_maxVel", 21, 1);
            p->midiLearn.assign("ui_sel_slotOfs", 22, 1);
            juce::AudioBuffer<float> a(2, 512); juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 20, 50), 0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 21, 100), 1);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 22, 90), 2);
            p->processBlock(a, midi);
            CHECK(p->selQHead.load() != p->selQTail.load());
            auto* editor = dynamic_cast<DrumSequencerEditor*>(ed.get());
            CHECK(editor != nullptr);
            if (editor) editor->timerCallback();
            CHECK(p->selQHead.load() == p->selQTail.load());
            CHECK(std::abs(c.keysMinVel - 50.0f / 127) < 1e-6);
            CHECK(std::abs(c.keysMaxVel - 100.0f / 127) < 1e-6);
            CHECK(std::abs(c.humanizeAmt - 90.0f / 127) < 1e-6);
            CHECK(s.patterns[0].channels[1].keysMinVel == 0); // CC remains per selected channel
            CHECK(s.patterns[1].channels[0].keysMinVel == 0); // existing per-pattern scope
            CHECK(std::abs(keys->minVelKnob.getValue() - 50.0 / 127) <= 0.005); // knob display has 1% steps
            c.slots[1].engine = -1;
            if (editor) editor->timerCallback();
            CHECK(!keys->humanKnob.isEnabled());
        }
        p->releaseResources();
    }
    printf("LiveDrumTest: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
