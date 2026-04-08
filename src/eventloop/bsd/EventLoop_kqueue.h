#pragma once

#include <EventLoop.h>

#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

class KQueueEventLoop : public EventLoop {
public:
    KQueueEventLoop();
    ~KQueueEventLoop() override;

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

private:
    void processTimers();

    int kqFd_       = -1;
    int wakeupRead_  = -1;  // read end of self-pipe
    int wakeupWrite_ = -1;  // write end
    int watchFd_     = -1;  // open fd for file watch (EVFILT_VNODE)

    bool running_ = false;

    struct FdEntry {
        FdEvents events;
        FdCb     cb;
    };
    std::unordered_map<int, FdEntry> fds_;

    struct Timer {
        TimerId  id;
        uint64_t ms;
        bool     repeat;
        TimerCb  cb;
        uint64_t nextFireMs;  // monotonic milliseconds

        bool operator>(const Timer& o) const { return nextFireMs > o.nextFireMs; }
    };
    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers_;
    TimerId nextTimerId_ = 1;

    WatchCb     fileWatchCb_;
    std::string fileWatchPath_;

    static uint64_t nowMs();
};
