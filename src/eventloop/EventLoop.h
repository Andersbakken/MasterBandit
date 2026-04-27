#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

    // Post a callable to run on the main thread (the thread driving run()).
    // Thread-safe; queues + wakes the loop. The callable runs once, the
    // next time drainPosts() is invoked. The host (PlatformDawn::tick) is
    // responsible for calling drainPosts() at a well-defined point in each
    // iteration; concrete backends do not auto-drain.
    void post(std::function<void()> fn);

    // Run all queued post() callables on the calling thread. Called from
    // the host's per-iteration tick at the point where deferred main-thread
    // work should land.
    void drainPosts();

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

    // Called when the platform-native UI (e.g. macOS app menu's Quit, dock
    // Quit, or Cmd+Q routed through NSApp.terminate:) asks the app to exit.
    // PlatformDawn sets this to its own quit() so the cleanup path is shared
    // with the JS-driven mb.quit().
    std::function<void()> onQuitRequested;

private:
    std::mutex                          postMu_;
    std::vector<std::function<void()>>  posted_;
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

inline void EventLoop::post(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(postMu_);
        posted_.push_back(std::move(fn));
    }
    wakeup();
}

inline void EventLoop::drainPosts()
{
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(postMu_);
        local.swap(posted_);
    }
    for (auto& fn : local) fn();
}
