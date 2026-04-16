#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <eventloop/EventLoop.h>

// Owns the three main-thread timers that shape rendering cadence outside of
// normal tick-driven redraws:
//   1. Cursor blink: a repeating timer that fades the cursor opacity over a
//      configurable phase. The current opacity is snapshotted into
//      RenderFrameState at frame build time.
//   2. Animation wakeup: a one-shot timer scheduled from the render thread to
//      mark the next animated-image frame boundary. The render thread stashes
//      the due time in an atomic; the main thread consumes it and arms the
//      actual EventLoop timer.
//   3. Resize debounce: a short coalescing timer that delays the expensive
//      surface reconfigure / pane reflow work during a live window drag.
//
// All timer (de)registration happens on the main thread. Only
// scheduleAnimationAt() is safe to call from the render thread.
class AnimationScheduler {
public:
    struct Host {
        EventLoop* eventLoop = nullptr;
        std::function<void()> onRedraw;                         // setNeedsRedraw equivalent
        std::function<void()> onBlinkTick;                      // main thread, fired after opacity update
        std::function<void(uint32_t w, uint32_t h)> onResizeDebounceFire;  // main thread
    };

    explicit AnimationScheduler(Host host);
    ~AnimationScheduler();

    AnimationScheduler(const AnimationScheduler&) = delete;
    AnimationScheduler& operator=(const AnimationScheduler&) = delete;

    // Cursor blink
    void applyBlinkConfig(int rateMs, int fps);
    void resetBlink();
    float blinkOpacity() const { return blinkOpacity_; }

    // Animation wakeup — scheduleAnimationAt may be called from any thread.
    void scheduleAnimationAt(uint64_t dueAtNs);
    void applyPendingAnimation();

    // Resize debounce — main thread only.
    void scheduleResize(uint32_t w, uint32_t h);
    bool takePendingResize(uint32_t& outW, uint32_t& outH);

    // Cancel all active timers. Called during shutdown before the event loop
    // is torn down.
    void stopAllTimers();

private:
    Host host_;

    // Blink
    EventLoop::TimerId blinkTimer_ = 0;
    int blinkRate_ = 800;
    int blinkFps_ = 10;
    int blinkStep_ = 0;
    int blinkTotalSteps_ = 0;
    float blinkOpacity_ = 1.0f;

    // Animation wakeup
    EventLoop::TimerId animTimer_ = 0;
    uint64_t animDueAt_ = 0;
    std::atomic<uint64_t> pendingAnimDueAt_ { 0 };

    // Resize debounce
    EventLoop::TimerId resizeTimer_ = 0;
    uint32_t pendingResizeW_ = 0;
    uint32_t pendingResizeH_ = 0;
};
