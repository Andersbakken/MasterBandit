#pragma once

#include <EventLoop.h>
#include "../kqueue/FdPoller_kqueue.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid Objective-C headers polluting C++ TUs
#ifdef __OBJC__
@class NSTimer;
@class MBEventLoopDelegate;
#else
struct objc_object;
using NSTimer            = objc_object;
using MBEventLoopDelegate = objc_object;
#endif

class NSAppEventLoop : public EventLoop {
public:
    NSAppEventLoop();
    ~NSAppEventLoop() override;

    void run() override;
    void stop() override;
    void wakeup() override;

    void watchFd(int fd, FdEvents events, FdCb cb) override;
    void updateFd(int fd, FdEvents events) override;
    void removeFd(int fd) override;

    TimerId addTimer(uint64_t ms, bool repeat, TimerCb cb) override;
    void    removeTimer(TimerId id) override;
    void    restartTimer(TimerId id) override;

    void addFileWatch(const std::string& path, WatchCb cb) override;
    void removeFileWatch() override;

    // Called by the run loop observer / tick selector
    void tick();
    // Called when the FdPoller's kqueue fd becomes readable —
    // drains all pending fd events via fdPoller_->poll(0).
    void drainKqueue();
    // Called by an NSTimer
    void timerFired(TimerId id);
    // Called by FSEvents
    void fileChanged();

private:
    static FdPoller::Events toPoller(FdEvents ev);
    static FdEvents fromPoller(FdPoller::Events ev);

    // Per-fd watching delegates to FdPoller. Its kqueue fd is
    // wrapped in a CFFileDescriptor below so the CFRunLoop wakes
    // when any managed fd has events.
    std::unique_ptr<FdPollerKQueue> fdPoller_;
    // CFFileDescriptor wrapping fdPoller_->nativeHandle().
    void* kqCfFdRef_  = nullptr;  // CFFileDescriptorRef
    void* kqCfSource_ = nullptr;  // CFRunLoopSourceRef

    struct Timer {
        TimerId  id;
        uint64_t ms;
        bool     repeat;
        TimerCb  cb;
        NSTimer* nsTimer = nullptr;
    };
    std::vector<Timer> timers_;
    TimerId nextTimerId_ = 1;

    // FSEvents fires at the directory level; per-file dispatch happens
    // by stat-and-mtime-check inside each registered callback. The
    // FSEventStream is shared across all watches in the same parent
    // directory.
    struct WatchEntry { std::string path; WatchCb cb; };
    std::vector<WatchEntry> fileWatches_;
    std::string fileWatchDir_;
    void*       fsEventStream_ = nullptr;  // FSEventStreamRef

    void* observer_ = nullptr;  // CFRunLoopObserverRef
    // wakeup() may be called from the render thread; observer callback reads
    // on main. CFRunLoopWakeUp is documented thread-safe; we only need the
    // flag to be atomic.
    std::atomic<bool> wakeupPending_ { false };
};
