#pragma once

#include <cstdint>
#include <functional>
#include <string>

class EventLoop {
public:
    using TimerCb = std::function<void()>;
    using WatchCb = std::function<void()>;
    using TimerId = uint32_t;

    enum class FdEvents : uint8_t {
        Readable = 1,
        Writable = 2,
        ReadWrite = 3
    };
    using FdCb = std::function<void(FdEvents)>;

    virtual ~EventLoop() = default;

    virtual void run() = 0;
    virtual void stop() = 0;

    // Safe to call from any context (callbacks, other threads)
    virtual void wakeup() = 0;

    // fd watching — one registration per fd, events can be updated
    virtual void watchFd(int fd, FdEvents events, FdCb cb) = 0;
    virtual void updateFd(int fd, FdEvents events) = 0;
    virtual void removeFd(int fd) = 0;

    // Timers — ms=0 fires on next iteration, repeat=false for one-shot
    virtual TimerId addTimer(uint64_t ms, bool repeat, TimerCb cb) = 0;
    virtual void    removeTimer(TimerId id) = 0;
    virtual void    restartTimer(TimerId id) = 0;

    // File watching — single watch slot (config file only for now)
    virtual void addFileWatch(const std::string& path, WatchCb cb) = 0;
    virtual void removeFileWatch() = 0;

    // Called each iteration after waking, before sleeping again
    // Set by PlatformDawn to drive rendering
    std::function<void()> onTick;
};

inline EventLoop::FdEvents operator|(EventLoop::FdEvents a, EventLoop::FdEvents b)
{
    return static_cast<EventLoop::FdEvents>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool operator&(EventLoop::FdEvents a, EventLoop::FdEvents b)
{
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}
