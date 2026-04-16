#include "AnimationScheduler.h"

#include "Terminal.h"

#include <algorithm>

AnimationScheduler::AnimationScheduler(Host host)
    : host_(std::move(host))
{
}

AnimationScheduler::~AnimationScheduler()
{
    stopAllTimers();
}

void AnimationScheduler::stopAllTimers()
{
    if (!host_.eventLoop) return;
    if (blinkTimer_) {
        host_.eventLoop->removeTimer(blinkTimer_);
        blinkTimer_ = 0;
    }
    if (animTimer_) {
        host_.eventLoop->removeTimer(animTimer_);
        animTimer_ = 0;
    }
    animDueAt_ = 0;
    if (resizeTimer_) {
        host_.eventLoop->removeTimer(resizeTimer_);
        resizeTimer_ = 0;
    }
}

void AnimationScheduler::applyBlinkConfig(int rateMs, int fps)
{
    if (!host_.eventLoop) {
        blinkRate_ = rateMs;
        blinkFps_ = fps;
        return;
    }
    if (blinkTimer_) {
        host_.eventLoop->removeTimer(blinkTimer_);
        blinkTimer_ = 0;
    }
    blinkRate_ = rateMs;
    blinkFps_ = fps;
    blinkOpacity_ = 1.0f;
    blinkStep_ = 0;
    if (rateMs <= 0 || fps <= 0) return;

    int frameMs = 1000 / fps;
    int stepsPerPhase = std::max(1, rateMs / frameMs);
    blinkTotalSteps_ = stepsPerPhase * 2; // fade-out + fade-in

    blinkTimer_ = host_.eventLoop->addTimer(static_cast<uint64_t>(frameMs), true, [this]() {
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

        if (host_.onBlinkTick) host_.onBlinkTick();
    });
}

void AnimationScheduler::resetBlink()
{
    if (blinkRate_ <= 0 || !blinkTimer_ || !host_.eventLoop) return;
    blinkStep_ = 0;
    blinkOpacity_ = 1.0f;
    host_.eventLoop->restartTimer(blinkTimer_);
    if (host_.onRedraw) host_.onRedraw();
}

void AnimationScheduler::scheduleAnimationAt(uint64_t dueAtNs)
{
    // Called from the render thread during renderFrame(). The event loop
    // (addTimer / removeTimer) is not thread-safe, so we stash the request
    // in an atomic and let the main thread's onTick wire it up via
    // applyPendingAnimation(). Kick the event loop so onTick runs even when
    // nothing else is happening.
    pendingAnimDueAt_.store(dueAtNs, std::memory_order_release);
    if (host_.eventLoop) host_.eventLoop->wakeup();
}

void AnimationScheduler::applyPendingAnimation()
{
    // Main thread only. Consume the atomic request stashed by the render
    // thread and (re)arm the event loop timer.
    uint64_t dueAt = pendingAnimDueAt_.exchange(0, std::memory_order_acquire);
    if (!dueAt || !host_.eventLoop) return;

    uint64_t now = TerminalEmulator::mono();
    if (dueAt <= now) {
        if (animTimer_) {
            host_.eventLoop->removeTimer(animTimer_);
            animTimer_ = 0;
        }
        animDueAt_ = 0;
        if (host_.onRedraw) host_.onRedraw();
        return;
    }
    // Skip reschedule if an equivalent timer is already pending.
    if (animTimer_ && animDueAt_ == dueAt) return;
    if (animTimer_) {
        host_.eventLoop->removeTimer(animTimer_);
        animTimer_ = 0;
    }
    animDueAt_ = dueAt;
    uint64_t delay = dueAt - now;
    animTimer_ = host_.eventLoop->addTimer(delay, false, [this]() {
        animTimer_ = 0;
        animDueAt_ = 0;
        if (host_.onRedraw) host_.onRedraw();
    });
}

void AnimationScheduler::scheduleResize(uint32_t w, uint32_t h)
{
    pendingResizeW_ = w;
    pendingResizeH_ = h;
    if (!host_.eventLoop || resizeTimer_ != 0) return;
    resizeTimer_ = host_.eventLoop->addTimer(25, false, [this]() {
        resizeTimer_ = 0;
        uint32_t rw = pendingResizeW_;
        uint32_t rh = pendingResizeH_;
        pendingResizeW_ = 0;
        pendingResizeH_ = 0;
        if (rw && rh && host_.onResizeDebounceFire)
            host_.onResizeDebounceFire(rw, rh);
    });
}

bool AnimationScheduler::takePendingResize(uint32_t& outW, uint32_t& outH)
{
    if (resizeTimer_ && host_.eventLoop) {
        host_.eventLoop->removeTimer(resizeTimer_);
        resizeTimer_ = 0;
    }
    if (!pendingResizeW_ || !pendingResizeH_) return false;
    outW = pendingResizeW_;
    outH = pendingResizeH_;
    pendingResizeW_ = 0;
    pendingResizeH_ = 0;
    return true;
}
