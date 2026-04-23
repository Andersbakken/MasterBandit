#include "AnimationScheduler.h"

#include "PlatformDawn.h"
#include "Terminal.h"

#include <algorithm>

EventLoop* AnimationScheduler::eventLoop() const
{
    return platform_ ? platform_->eventLoop_.get() : nullptr;
}

AnimationScheduler::~AnimationScheduler()
{
    stopAllTimers();
}

void AnimationScheduler::stopAllTimers()
{
    EventLoop* el = eventLoop();
    if (!el) return;
    if (blinkTimer_) { el->removeTimer(blinkTimer_); blinkTimer_ = 0; }
    if (animTimer_)  { el->removeTimer(animTimer_);  animTimer_ = 0; }
    animDueAt_ = 0;
    if (resizeTimer_) { el->removeTimer(resizeTimer_); resizeTimer_ = 0; }
}

void AnimationScheduler::applyBlinkConfig(int rateMs, int fps)
{
    EventLoop* el = eventLoop();
    if (!el) {
        blinkRate_ = rateMs;
        blinkFps_ = fps;
        return;
    }
    if (blinkTimer_) { el->removeTimer(blinkTimer_); blinkTimer_ = 0; }
    blinkRate_ = rateMs;
    blinkFps_ = fps;
    blinkOpacity_ = 1.0f;
    blinkStep_ = 0;
    if (rateMs <= 0 || fps <= 0) return;

    int frameMs = 1000 / fps;
    int stepsPerPhase = std::max(1, rateMs / frameMs);
    blinkTotalSteps_ = stepsPerPhase * 2; // fade-out + fade-in

    blinkTimer_ = el->addTimer(static_cast<uint64_t>(frameMs), true, [this]() {
        blinkStep_ = (blinkStep_ + 1) % blinkTotalSteps_;
        int stepsPerPhase = blinkTotalSteps_ / 2;
        float t;
        if (blinkStep_ < stepsPerPhase) {
            // Fade-out phase: 1.0 → 0.0
            t = 1.0f - static_cast<float>(blinkStep_) / static_cast<float>(stepsPerPhase);
        } else {
            // Fade-in phase: 0.0 → 1.0
            t = static_cast<float>(blinkStep_ - stepsPerPhase) / static_cast<float>(stepsPerPhase);
        }
        blinkOpacity_ = t;

        platform_->onBlinkTick();
    });
}

void AnimationScheduler::resetBlink()
{
    EventLoop* el = eventLoop();
    if (blinkRate_ <= 0 || !blinkTimer_ || !el) return;
    blinkStep_ = 0;
    blinkOpacity_ = 1.0f;
    el->restartTimer(blinkTimer_);
    platform_->setNeedsRedraw();
}

void AnimationScheduler::scheduleAnimationAt(uint64_t dueAtNs)
{
    // Called from the render thread during renderFrame(). The event loop
    // (addTimer / removeTimer) is not thread-safe, so we stash the request
    // in an atomic and let the main thread's onTick wire it up via
    // applyPendingAnimation(). Kick the event loop so onTick runs even when
    // nothing else is happening.
    pendingAnimDueAt_.store(dueAtNs, std::memory_order_release);
    if (EventLoop* el = eventLoop()) el->wakeup();
}

void AnimationScheduler::applyPendingAnimation()
{
    // Main thread only. Consume the atomic request stashed by the render
    // thread and (re)arm the event loop timer.
    uint64_t dueAt = pendingAnimDueAt_.exchange(0, std::memory_order_acquire);
    EventLoop* el = eventLoop();
    if (!dueAt || !el) return;

    uint64_t now = TerminalEmulator::mono();
    if (dueAt <= now) {
        if (animTimer_) { el->removeTimer(animTimer_); animTimer_ = 0; }
        animDueAt_ = 0;
        platform_->setNeedsRedraw();
        return;
    }
    // Skip reschedule if an equivalent timer is already pending.
    if (animTimer_ && animDueAt_ == dueAt) return;
    if (animTimer_) { el->removeTimer(animTimer_); animTimer_ = 0; }
    animDueAt_ = dueAt;
    uint64_t delay = dueAt - now;
    animTimer_ = el->addTimer(delay, false, [this]() {
        animTimer_ = 0;
        animDueAt_ = 0;
        platform_->setNeedsRedraw();
    });
}

void AnimationScheduler::scheduleResize(uint32_t w, uint32_t h)
{
    pendingResizeW_ = w;
    pendingResizeH_ = h;
    EventLoop* el = eventLoop();
    if (!el || resizeTimer_ != 0) return;
    resizeTimer_ = el->addTimer(25, false, [this]() {
        resizeTimer_ = 0;
        uint32_t rw = pendingResizeW_;
        uint32_t rh = pendingResizeH_;
        pendingResizeW_ = 0;
        pendingResizeH_ = 0;
        if (rw && rh) platform_->onResizeDebounceFire(rw, rh);
    });
}

bool AnimationScheduler::takePendingResize(uint32_t& outW, uint32_t& outH)
{
    EventLoop* el = eventLoop();
    if (resizeTimer_ && el) {
        el->removeTimer(resizeTimer_);
        resizeTimer_ = 0;
    }
    if (!pendingResizeW_ || !pendingResizeH_) return false;
    outW = pendingResizeW_;
    outH = pendingResizeH_;
    pendingResizeW_ = 0;
    pendingResizeH_ = 0;
    return true;
}
