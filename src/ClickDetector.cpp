#include "ClickDetector.h"
#include <cmath>

bool ClickDetector::withinRadius(int x1, int y1, int x2, int y2) const
{
    int dx = x1 - x2, dy = y1 - y2;
    return (dx * dx + dy * dy) <= dragThreshold_ * dragThreshold_ * 4;
    // Generous radius for multi-click (4x the drag threshold squared)
}

ClickDetector::Result ClickDetector::onPress(MouseButton button, int pixelX, int pixelY)
{
    int bi = idx(button);
    auto now = Clock::now();
    auto& rec = lastPress_[bi];

    buttonDown_[bi] = true;
    dragStarted_ = false;
    pressX_ = pixelX;
    pressY_ = pixelY;
    activeButton_ = button;

    // Check if this press continues a multi-click sequence
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - rec.time);
    bool continuing = rec.count > 0 &&
                      elapsed <= clickInterval_ &&
                      withinRadius(pixelX, pixelY, rec.x, rec.y);

    if (continuing && rec.count < 3) {
        rec.count++;
    } else {
        rec.count = 1;
    }
    rec.time = now;
    rec.x = pixelX;
    rec.y = pixelY;

    MouseEventType type;
    switch (rec.count) {
    case 2:  type = MouseEventType::DoublePress; break;
    case 3:  type = MouseEventType::TriplePress; break;
    default: type = MouseEventType::Press; break;
    }
    return {type, button};
}

ClickDetector::Result ClickDetector::onRelease(MouseButton button, int /*pixelX*/, int /*pixelY*/)
{
    int bi = idx(button);
    buttonDown_[bi] = false;

    MouseEventType type;
    if (!dragStarted_ && lastPress_[bi].count == 1) {
        type = MouseEventType::Click;
    } else {
        type = MouseEventType::Release;
    }
    dragStarted_ = false;
    return {type, button};
}

std::optional<ClickDetector::Result> ClickDetector::onMove(int pixelX, int pixelY)
{
    if (!buttonDown_[idx(activeButton_)]) return std::nullopt;
    if (dragStarted_) return std::nullopt; // already reported Drag

    int dx = pixelX - pressX_, dy = pixelY - pressY_;
    if (dx * dx + dy * dy > dragThreshold_ * dragThreshold_) {
        dragStarted_ = true;
        // Reset click count — drag cancels multi-click
        lastPress_[idx(activeButton_)].count = 0;
        return Result{MouseEventType::Drag, activeButton_};
    }
    return std::nullopt;
}
