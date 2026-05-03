#pragma once

#include <EventLoop.h>
#include "FdPoller_kqueue.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    void    restartTimer(TimerId id) override;

    void addFileWatch(const std::string& path, WatchCb cb) override;
    void removeFileWatch() override;

private:
    void processTimers();
    static FdPoller::Events toPoller(FdEvents ev);
    static FdEvents fromPoller(FdPoller::Events ev);

    // Outer kqueue: holds the wakeup self-pipe, EVFILT_VNODE
    // file watches, and the FdPoller's kqueue fd as a sub-source.
    int kqFd_       = -1;
    int wakeupRead_  = -1;  // read end of self-pipe
    int wakeupWrite_ = -1;  // write end

    // Per-fd readability/writability watching. The FdPoller's own
    // kqueue fd is registered with kqFd_ above, so when any
    // managed fd becomes ready the outer run loop wakes and we
    // call fdPoller_->poll(0) to drain.
    std::unique_ptr<FdPollerKQueue> fdPoller_;

    bool running_ = false;

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

    // File watches always observe the parent directory (one
    // EVFILT_VNODE/NOTE_WRITE watch per unique parent dir, shared by
    // every WatchEntry under that dir) — never the file itself. This
    // mirrors what the epoll backend does with inotify-on-dir and what
    // FSEvents does on macOS, and means callers can register a watch
    // on a path that is created OR deleted OR re-created after launch.
    //
    // Trade-off: NOTE_WRITE on a directory wakes us for ANY change to
    // that directory's children, so the dispatch in drainKqueue does a
    // re-stat / mtime compare per entry to decide whether to fire the
    // user callback. With ~1-2 watches per app this is trivially cheap.
    struct WatchEntry {
        std::string path;        // absolute file path being observed
        std::string dir;         // parent directory (cached)
        WatchCb     cb;
        // Last observed mtime + size so we can suppress callbacks on
        // dir-change events that didn't actually touch this file. -1
        // means "file did not exist on the previous check"; that
        // sentinel is also the initial value so file creation always
        // counts as a change.
        time_t      lastMtime = -1;
        off_t       lastSize  = -1;
    };
    std::vector<WatchEntry> fileWatches_;

    // One open fd per parent directory. fds_ in this map are the kqueue
    // ident the EVFILT_VNODE watch was armed against. Refcounted by the
    // number of WatchEntry's whose `dir` matches.
    struct DirWatch {
        int fd = -1;
        int refCount = 0;
    };
    std::unordered_map<std::string, DirWatch> dirWatches_;

    // Internal: ensure a parent-dir watch exists for `dir`, bumping
    // refcount. Returns the fd or -1 on failure.
    int retainDirWatch(const std::string& dir);
    // Internal: drop a refcount, close the fd if it hits zero.
    void releaseDirWatch(const std::string& dir);
    // Internal: stat-and-fire helper used by drainKqueue when a dir
    // event arrives.
    void recheckEntry(WatchEntry& w);

    static uint64_t nowMs();
};
