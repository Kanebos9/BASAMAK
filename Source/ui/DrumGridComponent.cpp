#include "DrumGridComponent.h"

void DrumGridComponent::update()
{
    {
        const juce::ScopedLock lock(proc.getCallbackLock());
        auto &s = proc.sequencer;
        head = s.groupHead(s.currentPattern);
        bars = s.groupEnd(head) - head + 1;
        recording = s.drums.recording;
        playBar = s.isCurrentlyPlaying ? s.playPattern : -1;
        playPos = s.barPos();
        mirror.clear();
        for (int p = head; p < head + bars; ++p)
        {
            const auto &l = s.drums.patterns[(size_t)p];
            for (int i = 0; i < l.count; ++i)
                mirror.push_back({p, i, l.hits[(size_t)i]});
        }
    }
    repaint();
}
float DrumGridComponent::xFor(const VisibleHit &h) const
{
    return (float)((h.pattern - head + h.hit.pos) / bars * getWidth());
}
double DrumGridComponent::timeAt(int x) const
{
    double t =
        juce::jlimit(0.0, std::nextafter((double)bars, 0.0), (double)x / juce::jmax(1, getWidth()) * bars);
    if (snap > 0)
        t = std::floor(t * snap + 0.5) / snap;
    return juce::jlimit(0.0, std::nextafter((double)bars, 0.0), t);
}
int DrumGridComponent::hitAt(juce::Point<int> p) const
{
    int ch = firstRow + p.y / juce::jmax(1, rowHeight);
    int best = -1;
    float dist = 9;
    for (int i = 0; i < (int)mirror.size(); ++i)
        if (mirror[(size_t)i].hit.channel == ch)
        {
            float dx = std::abs(xFor(mirror[(size_t)i]) - p.x);
            if (dx < dist)
            {
                dist = dx;
                best = i;
            }
        }
    return best;
}
void DrumGridComponent::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colour(0xff101625));
    for (int r = 0; r < rows; ++r)
    {
        g.setColour(juce::Colour(r % 2 ? 0xff182033 : 0xff141c2d));
        g.fillRect(0, r * rowHeight, getWidth(), rowHeight - 1);
    }
    const int div = snap > 0 ? snap : 16;
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < div; ++i)
        {
            float x = (float)((b + (double)i / div) / bars * getWidth());
            g.setColour(juce::Colour(i == 0 ? 0xff8391ad : (i % 4 == 0 ? 0xff414d65 : 0xff28334a)));
            g.drawVerticalLine((int)x, 0, (float)getHeight());
        }
    for (const auto &h : mirror)
    {
        const int row = h.hit.channel - firstRow;
        if (row < 0 || row >= rows)
            continue;
        const float x = xFor(h), y = (float)row * rowHeight;
        const float hh = 8 + h.hit.velocity * (rowHeight - 16);
        const bool sel = h.pattern == dragPattern && h.index == dragIndex;
        g.setColour(sel ? juce::Colour(0xffffffff)
                        : juce::Colour(0xffe8bf4d).withAlpha(0.35f + 0.65f * h.hit.velocity));
        g.fillRoundedRectangle(x - 3, y + rowHeight - hh - 4, 7, hh, 2);
        if (sel)
        {
            g.setFont(11);
            g.drawText(juce::String((int)std::lround(h.hit.velocity * 127)), (int)x + 6, (int)y + 2, 32, 15,
                       juce::Justification::left);
        }
    }
    if (playBar >= head && playBar < head + bars)
    {
        float x = (float)((playBar - head + playPos) / bars * getWidth());
        g.setColour(juce::Colour(0xff61e6be));
        g.drawLine(x, 0, x, (float)getHeight(), 2);
    }
    g.setColour(juce::Colour(0xff96a6c6));
    g.setFont(10);
    for (int b = 0; b < bars; ++b)
        g.drawText("Bar " + juce::String(head + b + 1), (int)((double)b / bars * getWidth()) + 5, 1, 65, 12,
                   juce::Justification::left);
}
void DrumGridComponent::mouseDown(const juce::MouseEvent &e)
{
    if (recording)
        return;
    grabKeyboardFocus();
    int i = hitAt(e.getPosition());
    if (e.mods.isPopupMenu())
    {
        if (i >= 0)
            editMenu(i);
        return;
    }
    if (beforeEdit)
        beforeEdit();
    const juce::ScopedLock lock(proc.getCallbackLock());
    down = e.getPosition();
    velocityDrag = e.mods.isShiftDown();
    if (i >= 0)
    {
        auto h = mirror[(size_t)i];
        dragPattern = h.pattern;
        dragIndex = h.index;
        grabVelocity = h.hit.velocity;
        grabX = xFor(h) - e.x;
    }
    else
    {
        int ch = juce::jlimit(0, 15, firstRow + e.y / rowHeight);
        double t = timeAt(e.x);
        dragPattern = head + (int)t;
        auto &l = proc.sequencer.drums.patterns[(size_t)dragPattern];
        dragIndex = l.count;
        if (!l.add({t - std::floor(t), ch, 0.8f, 0}))
        {
            dragIndex = -1;
            return;
        }
        grabVelocity = 0.8f;
        grabX = 0;
    }
    if (selectChannel && dragIndex >= 0)
        selectChannel(proc.sequencer.drums.patterns[(size_t)dragPattern].hits[(size_t)dragIndex].channel);
    update();
}
void DrumGridComponent::mouseDrag(const juce::MouseEvent &e)
{
    if (recording || dragIndex < 0)
        return;
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto &src = proc.sequencer.drums.patterns[(size_t)dragPattern];
    if (dragIndex >= src.count)
        return;
    auto h = src.hits[(size_t)dragIndex];
    if (velocityDrag)
    {
        src.hits[(size_t)dragIndex].velocity =
            juce::jlimit(1.0f / 127, 1.0f, grabVelocity + (down.y - e.y) / 100.0f);
    }
    else
    {
        double t = timeAt((int)(e.x + grabX));
        int p = head + (int)t;
        h.pos = t - std::floor(t);
        h.channel = juce::jlimit(0, 15, firstRow + juce::jlimit(0, rows - 1, e.y / rowHeight));
        if (p == dragPattern)
            src.hits[(size_t)dragIndex] = h;
        else
        {
            auto &dst = proc.sequencer.drums.patterns[(size_t)p];
            int n = dst.count;
            if (dst.add(h))
            {
                src.erase(dragIndex);
                dragPattern = p;
                dragIndex = n;
            }
        }
    }
    update();
}
void DrumGridComponent::mouseUp(const juce::MouseEvent &)
{
    update();
}
void DrumGridComponent::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w)
{
    if (recording)
        return;
    int i = hitAt(e.getPosition());
    if (i < 0)
        return;
    if (beforeEdit)
        beforeEdit();
    const juce::ScopedLock lock(proc.getCallbackLock());
    auto h = mirror[(size_t)i];
    auto &v = proc.sequencer.drums.patterns[(size_t)h.pattern].hits[(size_t)h.index].velocity;
    v = juce::jlimit(1.0f / 127, 1.0f, v + (w.deltaY > 0 ? 1 : -1) / 127.0f);
    update();
}
bool DrumGridComponent::keyPressed(const juce::KeyPress &k)
{
    if (k != juce::KeyPress::deleteKey && k != juce::KeyPress::backspaceKey)
        return false;
    if (recording || dragIndex < 0)
        return true;
    if (beforeEdit)
        beforeEdit();
    const juce::ScopedLock lock(proc.getCallbackLock());
    proc.sequencer.drums.patterns[(size_t)dragPattern].erase(dragIndex);
    dragIndex = -1;
    update();
    return true;
}
void DrumGridComponent::editMenu(int i)
{
    auto h = mirror[(size_t)i];
    juce::PopupMenu m;
    m.addItem(1, "Delete hit");
    juce::PopupMenu v;
    for (int x : {16, 32, 48, 64, 80, 96, 112, 127})
        v.addItem(100 + x, "Velocity " + juce::String(x));
    m.addSubMenu("Velocity", v);
    juce::PopupMenu p;
    for (int x : {-100, -50, 0, 50, 100})
        p.addItem(400 + x, x == 0
                               ? "Centre"
                               : juce::String(x < 0 ? "Left " : "Right ") + juce::String(std::abs(x)) + "%");
    m.addSubMenu("Pan", p);
    juce::Component::SafePointer<DrumGridComponent> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                    [safe, h](int r)
                    {
                        if (!safe || r == 0)
                            return;
                        auto &o = *safe;
                        if (o.beforeEdit)
                            o.beforeEdit();
                        const juce::ScopedLock lock(o.proc.getCallbackLock());
                        auto &d = o.proc.sequencer.drums;
                        if (d.recording)
                            return;
                        auto &l = d.patterns[(size_t)h.pattern];
                        if (h.index >= l.count)
                            return;
                        if (r == 1)
                            l.erase(h.index);
                        else if (r >= 300)
                            l.hits[(size_t)h.index].pan = (r - 400) / 100.0f;
                        else
                            l.hits[(size_t)h.index].velocity = (r - 100) / 127.0f;
                        o.dragIndex = -1;
                        o.update();
                    });
}
void DrumGridComponent::quantize()
{
    if (recording || snap <= 0)
        return;
    if (beforeEdit)
        beforeEdit();
    const juce::ScopedLock lock(proc.getCallbackLock());
    for (int p = head; p < head + bars; ++p)
    {
        auto &l = proc.sequencer.drums.patterns[(size_t)p];
        for (int i = 0; i < l.count; ++i)
            l.hits[(size_t)i].pos =
                juce::jmin(std::nextafter(1.0, 0.0), std::round(l.hits[(size_t)i].pos * snap) / snap);
    }
    update();
}
juce::String DrumGridComponent::getTooltip()
{
    return "LIVE DRUMMING: one row per sound channel; all rows share this timeline. Click to add; drag to "
           "move in time or to another channel. Shift-drag vertically or use the wheel over a hit to change "
           "velocity. Right-click for velocity, pan or delete; Delete removes the selected hit. Snap only "
           "affects edits, never your recorded timing. Editing is disabled during recording. MIDI "
           "assignments are in Routing > Channel > MIDI In.";
}
