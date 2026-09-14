#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

// Kit MIDI and performance data. Audio-thread writes use fixed storage; UI/state edits
// must hold the processor callback lock. The take log is drained by the editor under
// that same lock. MIDI note numbers select instruments, never their pitch.
class LiveDrumming
{
  public:
    static constexpr int CHANNELS = 16, PATTERNS = 64, MAX_HITS = 2048;
    static constexpr int INPUT_CAP = 2048, MAX_LOG = 65536, MAX_TAKES = 1000;
    struct Hit
    {
        double pos = 0;
        int channel = 0;
        float velocity = 1, pan = 0;
    };
    struct Lane
    {
        std::array<Hit, MAX_HITS> hits{};
        int count = 0;
        bool add(Hit h)
        {
            if (count >= MAX_HITS || !std::isfinite(h.pos) || h.channel < 0 || h.channel >= CHANNELS)
                return false;
            h.pos = juce::jlimit(0.0, std::nextafter(1.0, 0.0), h.pos);
            h.velocity = std::isfinite(h.velocity) ? juce::jlimit(1.0f / 127, 1.0f, h.velocity) : 1;
            h.pan = std::isfinite(h.pan) ? juce::jlimit(-1.0f, 1.0f, h.pan) : 0;
            hits[(size_t)count++] = h;
            return true;
        }
        void erase(int i)
        {
            if (i >= 0 && i < count)
            {
                for (int j = i + 1; j < count; ++j)
                    hits[(size_t)j - 1] = hits[(size_t)j];
                --count;
            }
        }
    };
    struct Input
    {
        int channel, offset;
        float velocity;
        bool consumed = false;
    };
    struct RecordedHit
    {
        int pattern = 0;
        Hit hit;
    };
    struct Take
    {
        juce::String name;
        int head = 0, bars = 1;
        std::vector<RecordedHit> hits;
    };
    struct LogEvent
    {
        int pattern = 0;
        Hit hit;
        int head = 0, bars = 1;
        bool boundary = false;
    };

    bool enabled = false;
    std::array<Lane, PATTERNS> patterns;
    std::array<int, CHANNELS> notes;
    std::array<int, CHANNELS> midiChannels{}; // 0 = any; 1..16 = exact MIDI channel
    int learnChannel = -1;
    int lastNote = -1, lastMidiChannel = 0, lastVelocity = 0;
    uint32_t activity = 0;
    std::array<uint32_t, CHANNELS> flashes{};
    std::array<Input, INPUT_CAP> input{};
    int inputCount = 0;
    bool inputOverflow = false, recordOverflow = false;
    std::atomic<bool> recording { false }; // UI guards can observe an audio-thread limit/transport stop.
    bool followChain = false;
    int armedHead = 0, armedEnd = 0, passHead = -1, passBars = 1, passEpoch = -1, passLastPattern = -1;
    std::array<LogEvent, MAX_LOG> log{};
    int logCount = 0;
    std::vector<Take> takes; // message thread / callback lock only
    Take pendingTake;

    LiveDrumming()
    {
        defaultMap();
    }
    void defaultMap()
    {
        // Match BASAMAK's initial channel labels; remaining rows cover the OKTO's
        // other documented notes. Learn always follows the user's actual kit.
        notes = {{36, 38, 42, 46, 39, 45, 49, 37, 43, 48, 51, 44, -1, -1, -1, -1}};
        midiChannels.fill(0);
        learnChannel = -1;
    }
    void assign(int ch, int note, int midiChannel)
    {
        if (ch < 0 || ch >= CHANNELS)
            return;
        note = juce::jlimit(-1, 127, note);
        midiChannel = juce::jlimit(0, 16, midiChannel);
        // An overlapping assignment moves to this row; one pad never accidentally doubles.
        for (int c = 0; c < CHANNELS; ++c)
            if (c != ch && notes[(size_t)c] == note &&
                (midiChannel == 0 || midiChannels[(size_t)c] == 0 || midiChannels[(size_t)c] == midiChannel))
                notes[(size_t)c] = -1;
        notes[(size_t)ch] = note;
        midiChannels[(size_t)ch] = midiChannel;
    }
    int target(int note, int midiChannel) const
    {
        for (int c = 0; c < CHANNELS; ++c)
            if (notes[(size_t)c] == note &&
                (midiChannels[(size_t)c] == 0 || midiChannels[(size_t)c] == midiChannel))
                return c;
        return -1;
    }
    void beginBlock()
    {
        inputCount = 0;
    }
    void noteOn(int note, int midiChannel, int velocity, int offset)
    {
        lastNote = note;
        lastMidiChannel = midiChannel;
        lastVelocity = velocity;
        ++activity;
        if (learnChannel >= 0)
        {
            assign(learnChannel, note, midiChannel);
            learnChannel = -1;
            return;
        }
        const int ch = target(note, midiChannel);
        if (ch < 0)
            return;
        if (inputCount >= INPUT_CAP)
        {
            inputOverflow = true;
            return;
        }
        input[(size_t)inputCount++] = {ch, offset, (float)velocity / 127.0f, false};
        ++flashes[(size_t)ch];
    }
    void clearNotes()
    {
        for (auto &p : patterns)
            p.count = 0;
    }
    bool hasNotes() const
    {
        for (const auto &p : patterns)
            if (p.count)
                return true;
        return false;
    }
    void arm(int head, int end, bool follow)
    {
        armedHead = juce::jlimit(0, PATTERNS - 1, head);
        armedEnd = juce::jlimit(armedHead, PATTERNS - 1, end);
        followChain = follow;
        passHead = -1;
        passEpoch = -1;
        passLastPattern = -1;
        logCount = 0;
        pendingTake = {};
        recordOverflow = false;
        recording = true;
    }
    void endPass()
    {
        if (passHead >= 0)
        {
            if (logCount < MAX_LOG)
                log[(size_t)logCount++] = {0, {}, passHead, passBars, true};
            else
                recordOverflow = true;
        }
        passHead = -1;
    }
    void stopRecording()
    {
        endPass();
        recording = false;
    }
    void enterSegment(int pattern, int epoch)
    {
        if (!recording)
            return;
        const bool newPass =
            passHead < 0 || (epoch != passEpoch && (followChain || pattern <= passLastPattern ||
                                                    pattern < armedHead || pattern > armedEnd));
        if (newPass)
        {
            endPass();
            passHead = followChain ? pattern : armedHead;
            passBars = followChain ? 1 : armedEnd - armedHead + 1;
            for (int p = passHead; p < passHead + passBars; ++p)
                patterns[(size_t)p].count = 0;
        }
        passEpoch = epoch;
        passLastPattern = pattern;
    }
    void record(int pattern, Hit hit)
    {
        if (!recording)
            return;
        // Leave room for the final boundary. Never silently erase a take on overflow.
        if (logCount >= MAX_LOG - 1 || patterns[(size_t)pattern].count >= MAX_HITS)
        {
            recordOverflow = true;
            stopRecording();
            return;
        }
        patterns[(size_t)pattern].add(hit);
        log[(size_t)logCount++] = {pattern, hit, passHead, passBars, false};
    }
    int takesFor(int head) const
    {
        int n = 0;
        for (const auto &t : takes)
            if (t.head == head)
                ++n;
        return n;
    }
    void drainLog()
    {
        for (int i = 0; i < logCount; ++i)
        {
            const auto &e = log[(size_t)i];
            if (!e.boundary)
            {
                pendingTake.head = e.head;
                pendingTake.bars = e.bars;
                pendingTake.hits.push_back({e.pattern, e.hit});
            }
            else if (!pendingTake.hits.empty())
            {
                if ((int)takes.size() < MAX_TAKES && takesFor(pendingTake.head) < 20)
                {
                    pendingTake.name = "Kit take " + juce::String((int)takes.size() + 1);
                    takes.push_back(std::move(pendingTake));
                }
                else
                {
                    recording = false;
                    recordOverflow = true;
                }
                pendingTake = {};
            }
        }
        logCount = 0;
    }
    bool loadTake(int index)
    {
        if (recording || index < 0 || index >= (int)takes.size())
            return false;
        const auto &t = takes[(size_t)index];
        for (int p = t.head; p < juce::jmin(PATTERNS, t.head + t.bars); ++p)
            patterns[(size_t)p].count = 0;
        for (const auto &h : t.hits)
            if (h.pattern >= t.head && h.pattern < juce::jmin(PATTERNS, t.head + t.bars))
                patterns[(size_t)h.pattern].add(h.hit);
        return true;
    }
    juce::ValueTree save() const;
    void restore(const juce::ValueTree &);
};
