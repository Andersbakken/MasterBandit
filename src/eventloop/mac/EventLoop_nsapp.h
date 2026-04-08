#pragma once

#include "../EventLoop.h"

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

    void addFileWatch(const std::string& path, WatchCb cb) override;
    void removeFileWatch() override;

    // Called by the run loop observer / tick selector
    void tick();
    // Called by a CFRunLoop source when a fd becomes ready
    void fdReady(int fd, FdEvents events);
    // Called by an NSTimer
    void timerFired(TimerId id);
    // Called by FSEvents
    void fileChanged();

public:
    struct FdEntry {
        FdEvents events;
        FdCb     cb;
        void*    cfSource = nullptr;  // CFRunLoopSourceRef
        void*    cfFdRef  = nullptr;  // CFFileDescriptorRef
    };
private:
    std::unordered_map<int, FdEntry> fds_;

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
};
