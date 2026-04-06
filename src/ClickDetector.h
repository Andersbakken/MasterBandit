#pragma once

#include "InputTypes.h"
#include <chrono>
#include <optional>

class ClickDetector {
public:
    struct Result {
        MouseEventType type;
        MouseButton    button;
    };

    // Raw GLFW button press → synthesized event type (Press/DoublePress/TriplePress)
    Result onPress(MouseButton button, int pixelX, int pixelY);

    // Raw GLFW button release → Click (if no drag and single press) or Release
    Result onRelease(MouseButton button, int pixelX, int pixelY);

    // Cursor move while button is down → Drag on first move beyond threshold, nullopt after
    std::optional<Result> onMove(int pixelX, int pixelY);

    void setClickInterval(std::chrono::milliseconds ms) { clickInterval_ = ms; }
    void setDragThreshold(int pixels) { dragThreshold_ = pixels; }

private:
    using Clock = std::chrono::steady_clock;

    struct PressRecord {
        Clock::time_point time{};
        int x = 0, y = 0;
        int count = 0;
    };

    static constexpr int kButtonCount = 3;
    PressRecord lastPress_[kButtonCount]{};
    bool buttonDown_[kButtonCount]{};
    bool dragStarted_ = false;
    int pressX_ = 0, pressY_ = 0;
    MouseButton activeButton_ = MouseButton::Left;

    std::chrono::milliseconds clickInterval_{400};
    int dragThreshold_ = 3;

    static int idx(MouseButton b) { return static_cast<int>(b); }
    bool withinRadius(int x1, int y1, int x2, int y2) const;
};
