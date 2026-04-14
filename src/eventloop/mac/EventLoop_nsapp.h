#pragma once

#include <EventLoop.h>

#include <atomic>
#include <cstdint>
#include <functional>
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
    // Called when the kqueue fd becomes readable — drains all pending events
    void drainKqueue();
    // Called by an NSTimer
    void timerFired(TimerId id);
    // Called by FSEvents
    void fileChanged();

private:
    struct FdEntry {
        FdEvents events;
        FdCb     cb;
    };
    std::unordered_map<int, FdEntry> fds_;

    // kqueue fd — all watched fds are registered here
    int kqFd_ = -1;
    // Single CFFileDescriptor wrapping kqFd_
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

    WatchCb fileWatchCb_;
    void*   fsEventStream_ = nullptr;  // FSEventStreamRef

    void* observer_ = nullptr;  // CFRunLoopObserverRef
    // wakeup() may be called from the render thread; observer callback reads
    // on main. CFRunLoopWakeUp is documented thread-safe; we only need the
    // flag to be atomic.
    std::atomic<bool> wakeupPending_ { false };
};
