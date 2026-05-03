#pragma once

#include <EventLoop.h>
#include "FdPoller_epoll.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    static FdPoller::Events toPoller(FdEvents ev);
    static FdEvents fromPoller(FdPoller::Events ev);

    // Outer epoll: holds the wakeup eventfd, the timerfd, the
    // inotify fd, and the FdPoller's epoll fd as a sub-source.
    // Per-fd watches go through fdPoller_.
    int epollFd_ = -1;
    int wakeupFd_ = -1;   // eventfd
    int timerFd_  = -1;   // timerfd_create(CLOCK_MONOTONIC)
    int inotifyFd_ = -1;
    int inotifyWd_ = -1;  // watch descriptor for config dir
    std::string inotifyDir_; // dir we're currently watching, "" if none

    std::unique_ptr<FdPollerEpoll> fdPoller_;
    int innerPollerFd_ = -1;  // cached fdPoller_->nativeHandle()

    bool running_ = false;

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

    // (filename, cb) pairs sharing a single inotify-on-dir watch. The
    // caller-facing addFileWatch API allows multiple files but only if
    // they share a parent directory; addFileWatch on a file in a
    // different parent than the existing watches drops the existing set
    // (previous behavior was the same — see EventLoop.h).
    struct WatchEntry { std::string name; WatchCb cb; };
    std::vector<WatchEntry> fileWatches_;

    static uint64_t nowNs();
};
