#include "PluginProcessor.h"

bool DrumSequencerProcessor::hasDrummingSwitchData()
{
    const juce::ScopedLock lock(getCallbackLock());
    const auto &d = sequencer.drums;
    if (!keysTakes.empty() || !d.takes.empty() || !d.pendingTake.hits.empty() || d.logCount > 0 ||
        (keysDrawTakeReady.load() && keysDrawTakeCount > 0))
        return true;
    for (const auto &lane : d.patterns)
        if (lane.count > 0)
            return true;
    for (const auto &pattern : sequencer.patterns)
        for (const auto &ch : pattern.channels)
        {
            if (ch.drawNoteCount > 0)
                return true;
            for (bool on : ch.steps)
                if (on)
                    return true;
        }
    return false;
}

// Conversion is prepared entirely before mutation. A capacity failure leaves the project
// untouched; the UI can explain it and offer Start fresh or Cancel instead.
bool DrumSequencerProcessor::switchDrumming(bool enable, bool convert, juce::String &error)
{
    const juce::ScopedLock lock(getCallbackLock());
    auto &d = sequencer.drums;
    if (d.enabled == enable)
        return true;
    if (keysRecording.load() || d.recording)
    {
        error = "Stop recording before switching modes.";
        return false;
    }
    std::vector<LiveDrumming::RecordedHit> converted;
    std::vector<LiveDrumming::Take> kitTakes;
    std::vector<KeysTake> regularTakes;
    std::array<int, Sequencer::NUM_PATTERNS> totals{};
    auto addHit =
        [&](std::vector<LiveDrumming::RecordedHit> &out, int p, int ch, double pos, float vel, float pan)
    {
        if (p < 0 || p >= Sequencer::NUM_PATTERNS)
            return;
        pos = juce::jlimit(0.0, std::nextafter(1.0, 0.0), pos);
        out.push_back({p, {pos, ch, juce::jlimit(1.0f / 127, 1.0f, vel), pan}});
    };
    auto collapseChords = [](std::vector<LiveDrumming::RecordedHit> &hits)
    {
        std::sort(hits.begin(), hits.end(),
                  [](const auto &a, const auto &b)
                  {
                      if (a.pattern != b.pattern)
                          return a.pattern < b.pattern;
                      if (a.hit.channel != b.hit.channel)
                          return a.hit.channel < b.hit.channel;
                      return a.hit.pos < b.hit.pos;
                  });
        size_t count = 0;
        for (const auto &h : hits)
        {
            if (count > 0 && hits[count - 1].pattern == h.pattern &&
                hits[count - 1].hit.channel == h.hit.channel &&
                std::abs(hits[count - 1].hit.pos - h.hit.pos) < 1.0e-9)
                hits[count - 1].hit.velocity = juce::jmax(hits[count - 1].hit.velocity, h.hit.velocity);
            else
                hits[count++] = h;
        }
        hits.resize(count);
    };
    auto fromNote =
        [&](std::vector<LiveDrumming::RecordedHit> &out, int base, int ch, const DrumChannel::DrawNote &n)
    {
        const int p = base + (int)(n.start / DrumChannel::DRAW_RES);
        const float pan = n.pan == DrumChannel::PAN_INHERIT
                              ? sequencer.patterns[juce::jlimit(0, 63, p)].channels[ch].drawPan
                              : n.pan * 0.01f;
        addHit(out, p, ch, std::fmod(n.start, DrumChannel::DRAW_RES) / DrumChannel::DRAW_RES, n.vel / 255.0f,
               pan);
    };
    auto toNote = [](const LiveDrumming::Hit &h, int barOffset)
    {
        DrumChannel::DrawNote n;
        n.start = (barOffset + h.pos) * DrumChannel::DRAW_RES;
        n.len = 1;
        n.semi = 0;
        n.vel = (uint8_t)juce::jlimit(1, 255, (int)std::lround(h.velocity * 255));
        n.pan = (int8_t)std::lround(h.pan * 100);
        n.oneShot = n.drumHit = 1;
        return n;
    };
    if (convert && enable)
    {
        for (int p = 0; p < Sequencer::NUM_PATTERNS; ++p)
            for (int ch = 0; ch < Sequencer::NUM_CHANNELS; ++ch)
            {
                const auto &c = sequencer.patterns[p].channels[ch];
                if (c.drawMode)
                {
                    for (int i = 0; i < c.drawNoteCount; ++i)
                        fromNote(converted, p, ch, c.drawNotes[i]);
                }
                else
                    for (int s = 0; s < c.numSteps; ++s)
                    {
                        if (!c.steps[s] || c.stepMerge[s])
                            continue;
                        double start, end;
                        Sequencer::stepSpan(s, c.numSteps, sequencer.patterns[p].swing, start, end);
                        const int rolls = juce::jmax(1, c.stepRoll[s]);
                        for (int r = 0; r < rolls; ++r)
                        {
                            double pos = juce::jlimit(0.0, 0.9999995,
                                                      start + (end - start) * r / rolls +
                                                          c.stepNudge[s] * 0.5 * (end - start));
                            const float rr = juce::jlimit(-1.0f, 1.0f, c.stepRollDecay[s]);
                            const float frac = rolls > 1 ? (float)r / (float)(rolls - 1) : 0.0f;
                            const float ramp =
                                rolls > 1 ? (rr >= 0 ? 1 - rr + rr * frac : 1 + rr * frac) : 1.0f;
                            addHit(converted, p, ch, pos, c.stepVel[s] * ramp, c.stepPan[s]);
                        }
                    }
            }
        for (const auto &t : keysTakes)
        {
            LiveDrumming::Take kt;
            kt.name = t.name;
            kt.head = t.isDraw ? t.drawPat : (t.evts.empty() ? 0 : t.evts.front().pattern);
            kt.bars = sequencer.groupEnd(kt.head) - kt.head + 1;
            if (t.isDraw)
                for (const auto &n : t.drawNotes)
                    fromNote(kt.hits, t.drawPat, t.channel, n);
            else
                for (const auto &e : t.evts)
                {
                    if (e.pattern >= Sequencer::NUM_PATTERNS || (e.flags & 1))
                        continue;
                    const auto &c = sequencer.patterns[e.pattern].channels[t.channel];
                    addHit(kt.hits, e.pattern, t.channel, (double)e.step / juce::jmax(1, c.numSteps),
                           c.stepVel[juce::jlimit(0, 63, (int)e.step)], 0);
                }
            collapseChords(kt.hits);
            if (kt.hits.empty())
                continue;
            int lo = 63, hi = 0;
            for (const auto &h : kt.hits)
            {
                lo = juce::jmin(lo, h.pattern);
                hi = juce::jmax(hi, h.pattern);
            }
            // Preserve the take's silent bars as well as its hits.
            hi = juce::jmax(hi, kt.head + kt.bars - 1);
            lo = juce::jmin(lo, kt.head);
            kt.head = lo;
            kt.bars = hi - lo + 1;
            if (kt.bars > 8)
            {
                error =
                    "A saved take spans more than eight bars. Keep it in regular mode or choose Start fresh.";
                return false;
            }
            kitTakes.push_back(std::move(kt));
        }
        collapseChords(converted);
        for (const auto &h : converted)
            if (++totals[(size_t)h.pattern] > LiveDrumming::MAX_HITS)
            {
                error = "A bar exceeds the drum editor's 2048-hit capacity. Reduce its notes or choose Start "
                        "fresh.";
                return false;
            }
        totals.fill(0);
        for (const auto &t : kitTakes)
        {
            if (++totals[(size_t)t.head] > 20)
            {
                error = "Converting these channel takes would exceed 20 kit takes for a pattern. Remove "
                        "unwanted takes first, or choose Start fresh.";
                return false;
            }
            std::array<int, 64> perBar{};
            for (const auto &h : t.hits)
                if (++perBar[(size_t)h.pattern] > LiveDrumming::MAX_HITS)
                {
                    error = "A saved take exceeds the drum editor's per-bar capacity.";
                    return false;
                }
        }
    }
    else if (convert)
    {
        std::array<std::array<int, 16>, 64> counts{};
        for (int p = 0; p < 64; ++p)
            for (int i = 0; i < d.patterns[(size_t)p].count; ++i)
            {
                const auto h = d.patterns[(size_t)p].hits[(size_t)i];
                if (++counts[(size_t)p][(size_t)h.channel] > DrumChannel::DRAW_MAX_NOTES)
                {
                    error = "A channel has more than 256 hits in one bar, which will not fit in the regular "
                            "piano roll. Reduce its hits or choose Start fresh.";
                    return false;
                }
                converted.push_back({p, h});
            }
        counts = {};
        for (const auto &t : d.takes)
            for (int anchor = sequencer.groupHead(t.head); anchor < t.head + t.bars;
                 anchor = sequencer.groupEnd(anchor) + 1)
                for (int ch = 0; ch < 16; ++ch)
                {
                    // The regular take menu is anchored at the viewed GROUP head. Split a
                    // take across current groups when its original bars have been unmerged.
                    KeysTake kt;
                    const auto tag = " / Ch " + juce::String(ch + 1);
                    kt.name = t.name.endsWith(tag) ? t.name : t.name + tag;
                    kt.channel = ch;
                    kt.isDraw = true;
                    kt.drawPat = anchor;
                    const int end = sequencer.groupEnd(anchor);
                    std::array<int, 64> perBar{};
                    for (const auto &h : t.hits)
                        if (h.hit.channel == ch && h.pattern >= anchor && h.pattern <= end)
                        {
                            if (++perBar[(size_t)h.pattern] > DrumChannel::DRAW_MAX_NOTES)
                            {
                                error = "A saved kit take exceeds 256 hits per channel per bar in the "
                                        "regular piano roll.";
                                return false;
                            }
                            kt.drawNotes.push_back(toNote(h.hit, h.pattern - anchor));
                        }
                    if (kt.drawNotes.empty())
                        continue;
                    if (++counts[(size_t)anchor][(size_t)ch] > 20 || regularTakes.size() >= KEYS_TAKES_MAX)
                    {
                        error = "Converting kit takes would exceed the regular recorder's take limit. Remove "
                                "unwanted takes first, or choose Start fresh.";
                        return false;
                    }
                    regularTakes.push_back(std::move(kt));
                }
    }
    sequencer.stopStandalone();
    sequencer.reset();
    for (auto &p : sequencer.patterns)
        for (auto &c : p.channels)
        {
            c.clearStepData();
            c.clearDrawNotes();
            c.keyUp();
            c.fadeOutVoices(0.01f);
            if (!enable && convert)
                c.drawMode = true;
        }
    keysHeldCount = 0;
    keysHeldNote.store(-1);
    keysHeldMaskLo.store(0);
    keysHeldMaskHi.store(0);
    arpRoot = -1;
    arpSounding = -1;
    arpChan = -1;
    arpSoundingUi.store(-1);
    keyQTail.store(keyQHead.load());
    keysDrawTakeReady.store(false);
    keysEvtCount.store(0);
    d.clearNotes();
    d.takes.clear();
    d.pendingTake = {};
    d.logCount = 0;
    d.passHead = -1;
    d.learnChannel = -1;
    keysTakes.clear();
    if (convert && enable)
    {
        for (const auto &h : converted)
            d.patterns[(size_t)h.pattern].add(h.hit);
        d.takes = std::move(kitTakes);
    }
    else if (convert)
    {
        for (const auto &h : converted)
            sequencer.patterns[h.pattern].channels[h.hit.channel].addDrawNote(toNote(h.hit, 0));
        keysTakes = std::move(regularTakes);
    }
    d.enabled = enable;
    sequencer.recordLoopLock.store(false);
    return true;
}
