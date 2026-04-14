#pragma once

#include <EventLoop.h>

#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

class EpollEventLoop : public EventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

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

private:
    void updateTimerFd();
    void drainTimers();
    void drainInotify();
    void drainWakeup();

    int epollFd_ = -1;
    int wakeupFd_ = -1;   // eventfd
    int timerFd_  = -1;   // timerfd_create(CLOCK_MONOTONIC)
    int inotifyFd_ = -1;
    int inotifyWd_ = -1;  // watch descriptor for config file

    bool running_ = false;

    struct FdEntry {
        FdEvents events;
        FdCb cb;
    };
    std::unordered_map<int, FdEntry> fds_;

    struct Timer {
        TimerId id;
        uint64_t ms;
        bool repeat;
        TimerCb cb;
        uint64_t nextFireNs;  // monotonic nanoseconds

        bool operator>(const Timer& o) const { return nextFireNs > o.nextFireNs; }
    };
    // Min-heap by nextFireNs
    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers_;
    TimerId nextTimerId_ = 1;

    WatchCb fileWatchCb_;
    std::string fileWatchPath_;

    static uint64_t nowNs();
    static int epollEventsFor(FdEvents ev);
};
