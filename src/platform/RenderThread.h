#pragma once

#include "RenderSync.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class EventLoop;
class Terminal;

// Owns the render worker thread, the cross-thread mutation queues, and the
// synchronization primitives that connect the main thread to the render
// thread. Constructed by PlatformDawn early in its ctor, wired via
// setPlatform() after the collaborators exist, started from createTerminal(),
// stopped from ~PlatformDawn.
class PlatformDawn;

class RenderThread {
    // mutex()/pending()/renderState() are coordination primitives, not
    // public API. PlatformDawn and InputController are the only legitimate
    // callers — they reach in through their own `platform_->renderThread_`
    // pointer.
    friend class PlatformDawn;
    friend class InputController;

public:
    RenderThread();
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void setPlatform(PlatformDawn* p) { platform_ = p; }

    // Lifecycle
    void start();
    void stop();

    // Cross-thread wake signal
    void wake();

    // Main-thread post queue
    void postToMain(std::function<void()> fn);
    void drainDeferredMain();

    // Terminal exit queue
    void enqueueTerminalExit(Terminal* t);
    void drainPendingExits();

    // Main-thread per-tick synchronization step.
    // Acquires mutex_, drains exits, transfers pending_ flags into
    // renderState_, calls host_.buildRenderFrameState(), clears pending_.
    void applyPendingMutations();

    // Number of renderFrame() invocations the render thread has completed.
    // Used by Graveyard to defer destruction of Terminal-owning objects
    // until at least one frame has ended since they were staged — which
    // guarantees that any frame that held a raw pointer to them at stage
    // time has returned and released its local frameState_ references.
    // Incremented by the render thread at the end of each renderFrame via
    // notifyFrameCompleted(). Read by the main thread.
    uint64_t completedFrames() const {
        return completedFrames_.load(std::memory_order_acquire);
    }
    void notifyFrameCompleted() {
        completedFrames_.fetch_add(1, std::memory_order_release);
    }

private:
    // Shared state accessors — available to PlatformDawn via friendship.
    // Callers must hold mutex() except where noted by the existing thread
    // contract.
    //
    // Recursive because some main-thread structural mutations re-enter
    // through JS / Terminal callbacks while already holding the lock (e.g.
    // InputController::onKey holds it across keyPressEvent, which may fire
    // a popup input callback that closes the popup via
    // scbs.destroyPopup → reacquires this mutex to extract + graveyard).
    std::recursive_mutex& mutex() { return mutex_; }
    PendingMutations& pending() { return pending_; }
    RenderFrameState& renderState() { return renderState_; }

    void threadMain();

    PlatformDawn* platform_ = nullptr;

    // Coarse mutex serializing render-thread reads against main-thread
    // structural mutations (tab/pane/popup create/destroy, tab switch,
    // resize). Terminal state is separately protected by
    // TerminalEmulator::mutex(). Recursive — see the mutex() accessor.
    std::recursive_mutex            mutex_;
    std::mutex                      renderCvMutex_;
    std::condition_variable         renderCv_;
    std::atomic<bool>               renderWake_ { false };
    std::atomic<bool>               renderStop_ { false };
    std::thread                     thread_;

    // Main-thread-only mutation accumulator. Written at scattered call
    // sites without any lock; consumed by applyPendingMutations().
    PendingMutations                pending_;

    // Shadow copy of tab/pane structure for the render thread. Written by
    // applyPendingMutations() under mutex_, read by renderFrame() under
    // the same mutex.
    RenderFrameState                renderState_;

    // Deferred structural mutations from parse callbacks that mutate
    // PlatformDawn structural state (notably terminalExited). Pushed
    // without holding mutex_, drained under mutex_.
    std::mutex                      deferredExitMutex_;
    std::vector<Terminal*>          pendingExits_;

    // Generic main-thread post queue. Parse-time callbacks enqueue
    // lambdas; drainDeferredMain() runs them on the main thread after
    // parse completes.
    std::mutex                      deferredMainMutex_;
    std::vector<std::function<void()>> deferredMain_;

    // Frame-completion counter. See completedFrames() / notifyFrameCompleted().
    std::atomic<uint64_t>           completedFrames_ { 0 };
};
