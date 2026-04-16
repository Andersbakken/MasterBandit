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
// thread. Constructed by PlatformDawn early in its ctor (before any
// component whose Host captures mutex()/pending()/renderState()), wired
// via setHost() after the collaborators exist, started from
// createTerminal(), stopped from ~PlatformDawn.
class PlatformDawn;

class RenderThread {
    // PlatformDawn is the only legitimate caller of mutex()/pending()/
    // renderState(); those are coordination primitives, not public API.
    // Other components receive raw pointers into this state via their
    // Host wiring, which PlatformDawn sets up at construction time.
    friend class PlatformDawn;

public:
    struct Host {
        // Main-thread event loop; created lazily by createTerminal().
        std::function<EventLoop*()> eventLoop;

        // Invoked on the render thread each iteration after wake.
        // Currently wraps renderEngine_->device().Tick() + renderFrame().
        std::function<void()> onFrame;

        // Called from applyPendingMutations under mutex(), after pending_
        // flags are transferred into renderState_, to rebuild the tab/pane
        // shadow copy. Expected to be PlatformDawn::buildRenderFrameState().
        std::function<void()> buildRenderFrameState;

        // Route a drained terminal exit back to structural cleanup.
        std::function<void(Terminal*)> onTerminalExit;
    };

    RenderThread();
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void setHost(Host host) { host_ = std::move(host); }

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

private:
    // Shared state accessors — available to PlatformDawn via friendship.
    // Callers must hold mutex() except where noted by the existing thread
    // contract.
    std::mutex& mutex() { return mutex_; }
    PendingMutations& pending() { return pending_; }
    RenderFrameState& renderState() { return renderState_; }

    void threadMain();

    Host host_;

    // Coarse mutex serializing render-thread reads against main-thread
    // structural mutations (tab/pane/popup create/destroy, tab switch,
    // resize). Terminal state is separately protected by
    // TerminalEmulator::mutex().
    std::mutex                      mutex_;
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
};
