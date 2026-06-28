#include "MidiLearnManager.h"

void MidiLearnManager::startLearning(const juce::String& paramId, int forcedChannel)
{
    // Write the param/channel FIRST, then publish 'learning' with release ordering
    // so the audio thread that sees learning==true also sees these values.
    learningParamId = paramId;
    learningForcedChannel = forcedChannel;
    learning.store(true, std::memory_order_release);
}

void MidiLearnManager::stopLearning()
{
    // Message-thread cancel: safe to clear the string here.
    learning.store(false, std::memory_order_release);
    learningParamId.clear();
    learningForcedChannel = -1;
}

bool MidiLearnManager::processMidiMessage(const juce::MidiMessage& msg)
{
    if (!learning.load(std::memory_order_acquire)) return false;
    if (!msg.isController()) return false;

    int cc  = msg.getControllerNumber();
    int ch  = (learningForcedChannel > 0) ? learningForcedChannel : msg.getChannel();
    {
        // AUDIO thread: TRY-lock only (never block). If the message thread is mid-edit, leave learning ON and
        // capture on the NEXT CC instead. We never mutate the maps OR call listeners from the audio thread.
        const juce::ScopedTryLock stl(mapCS);
        if (! stl.isLocked()) return true;          // consumed (don't route as a control); retry the capture later
        assignLocked(learningParamId, cc, ch);      // map mutation only
    }
    learning.store(false, std::memory_order_release);
    // No listener call here: the editor's timer polls isLearning() and repaints the cleared learn ring itself.
    return true;
}

int MidiLearnManager::getCCForParam(const juce::String& paramId) const
{
    const juce::ScopedLock sl(mapCS);
    if (!paramToCC.contains(paramId)) return -1;
    return paramToCC[paramId].ccNumber;
}

int MidiLearnManager::getChannelForParam(const juce::String& paramId) const
{
    const juce::ScopedLock sl(mapCS);
    if (!paramToCC.contains(paramId)) return -1;
    return paramToCC[paramId].midiChannel;
}

void MidiLearnManager::setChannelForPrefix(const juce::String& prefix, int newChannel)
{
    juce::Array<juce::String> toMove;
    juce::Array<int> ccs;
    {
        const juce::ScopedLock sl(mapCS);
        for (auto it = paramToCC.begin(); it != paramToCC.end(); ++it)
            if (it.getKey().startsWith(prefix))
            {
                toMove.add(it.getKey());
                ccs.add(it.getValue().ccNumber);
            }
        for (int i = 0; i < toMove.size(); ++i)
            assignLocked(toMove[i], ccs[i], newChannel);   // keeps CC, changes channel
    }
    for (auto& p : toMove) listeners.call(&Listener::midiLearnAssignmentChanged, p);
}

juce::String MidiLearnManager::getParamForCC(int ccNumber, int midiChannel) const
{
    // AUDIO thread (routeCC): TRY-lock only. If contended (a rare message-thread edit), skip routing this CC
    // for one block rather than risk a HashMap read during a concurrent rehash.
    const juce::ScopedTryLock stl(mapCS);
    if (! stl.isLocked()) return {};
    int k = key(ccNumber, midiChannel);
    if (!ccToParam.contains(k)) return {};
    return ccToParam[k];
}

void MidiLearnManager::clearParam(const juce::String& paramId)
{
    {
        const juce::ScopedLock sl(mapCS);
        if (!paramToCC.contains(paramId)) return;
        auto& a = paramToCC.getReference(paramId);
        if (a.ccNumber >= 0)
            ccToParam.remove(key(a.ccNumber, a.midiChannel));
        paramToCC.remove(paramId);
    }
    listeners.call(&Listener::midiLearnAssignmentChanged, paramId);
}

// Map mutation only - the CALLER must hold mapCS, and we do NOT notify listeners here (so the audio-thread learn
// path can call this without touching the UI). Public assign() wraps this with the lock + a notification.
void MidiLearnManager::assignLocked(const juce::String& paramId, int cc, int midiCh)
{
    // Remove any existing assignment for this CC
    int k = key(cc, midiCh);
    if (ccToParam.contains(k))
    {
        juce::String old = ccToParam[k];
        paramToCC.remove(old);
    }

    // Remove old CC for this param
    if (paramToCC.contains(paramId))
    {
        auto& old = paramToCC.getReference(paramId);
        if (old.ccNumber >= 0)
            ccToParam.remove(key(old.ccNumber, old.midiChannel));
    }

    paramToCC.set(paramId, { cc, midiCh });
    ccToParam.set(k, paramId);
}

void MidiLearnManager::assign(const juce::String& paramId, int cc, int midiCh)
{
    { const juce::ScopedLock sl(mapCS); assignLocked(paramId, cc, midiCh); }
    listeners.call(&Listener::midiLearnAssignmentChanged, paramId);
}

void MidiLearnManager::clearAll()
{
    { const juce::ScopedLock sl(mapCS); paramToCC.clear(); ccToParam.clear(); }
    stopLearning();
    listeners.call(&Listener::midiLearnAssignmentChanged, juce::String());
}

juce::ValueTree MidiLearnManager::saveState() const
{
    const juce::ScopedLock sl(mapCS);
    juce::ValueTree tree("MidiLearn");
    for (auto it = paramToCC.begin(); it != paramToCC.end(); ++it)
    {
        juce::ValueTree entry("Assign");
        entry.setProperty("param",   it.getKey(),        nullptr);
        entry.setProperty("cc",      it.getValue().ccNumber,    nullptr);
        entry.setProperty("channel", it.getValue().midiChannel, nullptr);
        tree.appendChild(entry, nullptr);
    }
    return tree;
}

void MidiLearnManager::loadState(const juce::ValueTree& tree)
{
    {
        const juce::ScopedLock sl(mapCS);
        paramToCC.clear();
        ccToParam.clear();
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto entry = tree.getChild(i);
            juce::String pid = entry.getProperty("param");
            int cc  = entry.getProperty("cc");
            int ch  = entry.getProperty("channel");
            if (pid.isNotEmpty() && cc >= 0)
                assignLocked(pid, cc, ch);
        }
    }
    // No listener spam on bulk load - the editor refreshes from the restored state.
}
