#include "EventLoop_kqueue.h"

#include <spdlog/spdlog.h>

#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

static constexpr int MaxEvents = 64;

uint64_t KQueueEventLoop::nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL
         + static_cast<uint64_t>(ts.tv_nsec) / 1'000'000ULL;
}

// ---------- lifecycle ----------

KQueueEventLoop::KQueueEventLoop()
{
    kqFd_ = kqueue();
    if (kqFd_ < 0)
        throw std::runtime_error(std::string("kqueue: ") + strerror(errno));

    int pipefd[2];
    if (pipe(pipefd) < 0)
        throw std::runtime_error(std::string("pipe: ") + strerror(errno));
    wakeupRead_  = pipefd[0];
    wakeupWrite_ = pipefd[1];
    fcntl(wakeupRead_,  F_SETFL, O_NONBLOCK);
    fcntl(wakeupWrite_, F_SETFL, O_NONBLOCK);

    struct kevent ev{};
    EV_SET(&ev, wakeupRead_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);

    // Per-fd watching delegates to FdPoller. Add its kqueue fd to
    // our outer kqueue so any managed fd's readiness wakes our
    // run loop, at which point we drain the poller via poll(0).
    fdPoller_ = std::make_unique<FdPollerKQueue>();
    int innerFd = fdPoller_->nativeHandle();
    if (innerFd >= 0) {
        struct kevent kev{};
        EV_SET(&kev, innerFd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        kevent(kqFd_, &kev, 1, nullptr, 0, nullptr);
    }
}

KQueueEventLoop::~KQueueEventLoop()
{
    fileWatches_.clear();
    for (auto& [_, dw] : dirWatches_) {
        if (dw.fd >= 0) close(dw.fd);
    }
    dirWatches_.clear();
    fdPoller_.reset();  // close inner kqueue before outer
    if (wakeupRead_  >= 0) { close(wakeupRead_);  wakeupRead_  = -1; }
    if (wakeupWrite_ >= 0) { close(wakeupWrite_); wakeupWrite_ = -1; }
    if (kqFd_ >= 0) { close(kqFd_); kqFd_ = -1; }
}

FdPoller::Events KQueueEventLoop::toPoller(FdEvents ev)
{
    return static_cast<FdPoller::Events>(static_cast<uint8_t>(ev));
}

EventLoop::FdEvents KQueueEventLoop::fromPoller(FdPoller::Events ev)
{
    return static_cast<FdEvents>(static_cast<uint8_t>(ev));
}

// ---------- run / stop / wakeup ----------

void KQueueEventLoop::run()
{
    running_ = true;
    struct kevent events[MaxEvents];

    while (running_) {
        // Compute timeout from nearest timer
        struct timespec timeout{};
        struct timespec* timeoutPtr = nullptr;

        if (!timers_.empty()) {
            uint64_t now = nowMs();
            uint64_t next = timers_.top().nextFireMs;
            if (next <= now) {
                // Already expired, don't block
                timeout = { 0, 0 };
            } else {
                uint64_t diffMs = next - now;
                timeout.tv_sec  = static_cast<time_t>(diffMs / 1000);
                timeout.tv_nsec = static_cast<long>((diffMs % 1000) * 1'000'000);
            }
            timeoutPtr = &timeout;
        }

        int n = kevent(kqFd_, nullptr, 0, events, MaxEvents, timeoutPtr);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("KQueueEventLoop: kevent: {}", strerror(errno));
            break;
        }

        // FdPoller integration: if its kqueue fd became readable
        // it means at least one managed fd has events pending.
        // Drain it once per wake; keep the call outside the
        // per-event loop so multiple readiness events for the
        // poller fd in a single batch are coalesced.
        bool drainPoller = false;
        const int innerFd = fdPoller_ ? fdPoller_->nativeHandle() : -1;

        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);

            if (events[i].filter == EVFILT_READ && fd == wakeupRead_) {
                // Drain the wakeup pipe
                char buf[64];
                while (read(wakeupRead_, buf, sizeof(buf)) > 0) {}
            } else if (events[i].filter == EVFILT_READ && fd == innerFd) {
                drainPoller = true;
            } else if (events[i].filter == EVFILT_VNODE) {
                // Directory-write event. Find which dir this fd belongs
                // to, then re-stat every WatchEntry under that dir and
                // fire callbacks whose file's mtime/size actually
                // changed (or appeared/disappeared). Iterate by index
                // because callbacks may mutate fileWatches_ via
                // removeFileWatch (which clears everything).
                std::string changedDir;
                for (auto& [dir, dw] : dirWatches_) {
                    if (dw.fd == fd) { changedDir = dir; break; }
                }
                if (!changedDir.empty()) {
                    for (size_t j = 0; j < fileWatches_.size(); ) {
                        size_t before = fileWatches_.size();
                        if (fileWatches_[j].dir == changedDir) {
                            recheckEntry(fileWatches_[j]);
                        }
                        // If the callback called removeFileWatch the
                        // vector is empty and we should stop. If it
                        // shrank but didn't fully empty (which the
                        // current API doesn't allow, but be defensive)
                        // re-anchor by clamping the index.
                        if (fileWatches_.size() < before) {
                            if (fileWatches_.empty()) break;
                            // entries before j unchanged; entries at j
                            // may have shifted, so don't advance.
                            continue;
                        }
                        ++j;
                    }
                }
            }
            // Per-fd EVFILT_READ / EVFILT_WRITE events live on the
            // inner FdPoller's kqueue, not this one. Anything that
            // reaches here for a non-wakeup, non-VNODE fd indicates
            // a leaked subscription; ignore it.
        }

        if (drainPoller && fdPoller_) {
            fdPoller_->poll(0);  // non-blocking, dispatches callbacks
        }

        processTimers();

        if (onTick) onTick();
    }
}

void KQueueEventLoop::stop()
{
    running_ = false;
    wakeup();
}

void KQueueEventLoop::wakeup()
{
    char b = 1;
    write(wakeupWrite_, &b, 1);
}

// ---------- fd watching (delegated to FdPoller) ----------

void KQueueEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    if (!fdPoller_) return;
    auto inner = std::move(cb);
    fdPoller_->add(fd, toPoller(events),
        [inner = std::move(inner)](FdPoller::Events ev) {
            if (inner) inner(fromPoller(ev));
        });
}

void KQueueEventLoop::updateFd(int fd, FdEvents events)
{
    if (!fdPoller_) return;
    fdPoller_->update(fd, toPoller(events));
}

void KQueueEventLoop::removeFd(int fd)
{
    if (!fdPoller_) return;
    fdPoller_->remove(fd);
}

// ---------- timers ----------

void KQueueEventLoop::processTimers()
{
    uint64_t now = nowMs();
    while (!timers_.empty() && timers_.top().nextFireMs <= now) {
        Timer t = timers_.top(); timers_.pop();
        t.cb();
        if (t.repeat) {
            t.nextFireMs = nowMs() + t.ms;
            timers_.push(std::move(t));
        }
    }
}

KQueueEventLoop::TimerId KQueueEventLoop::addTimer(uint64_t ms, bool repeat, TimerCb cb)
{
    TimerId id = nextTimerId_++;
    timers_.push({ id, ms, repeat, std::move(cb), nowMs() + ms });
    return id;
}

void KQueueEventLoop::removeTimer(TimerId id)
{
    std::vector<Timer> remaining;
    remaining.reserve(timers_.size());
    while (!timers_.empty()) {
        Timer t = timers_.top(); timers_.pop();
        if (t.id != id) remaining.push_back(std::move(t));
    }
    for (auto& t : remaining) timers_.push(std::move(t));
}

void KQueueEventLoop::restartTimer(TimerId id)
{
    uint64_t now = nowMs();
    std::vector<Timer> remaining;
    remaining.reserve(timers_.size());
    while (!timers_.empty()) {
        Timer t = timers_.top(); timers_.pop();
        if (t.id == id) {
            t.nextFireMs = now + t.ms;
        }
        remaining.push_back(std::move(t));
    }
    for (auto& t : remaining) timers_.push(std::move(t));
}

// ---------- file watching ----------

int KQueueEventLoop::retainDirWatch(const std::string& dir)
{
    auto it = dirWatches_.find(dir);
    if (it != dirWatches_.end()) {
        ++it->second.refCount;
        return it->second.fd;
    }
    int fd = open(dir.c_str(), O_RDONLY | O_EVTONLY);
    if (fd < 0) {
        spdlog::warn("KQueueEventLoop: addFileWatch open dir '{}': {}",
                     dir, strerror(errno));
        return -1;
    }
    struct kevent ev{};
    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE, 0, nullptr);
    kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
    dirWatches_.emplace(dir, DirWatch{fd, 1});
    return fd;
}

void KQueueEventLoop::releaseDirWatch(const std::string& dir)
{
    auto it = dirWatches_.find(dir);
    if (it == dirWatches_.end()) return;
    if (--it->second.refCount > 0) return;
    if (it->second.fd >= 0) {
        struct kevent ev{};
        EV_SET(&ev, it->second.fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
        kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
        close(it->second.fd);
    }
    dirWatches_.erase(it);
}

void KQueueEventLoop::recheckEntry(WatchEntry& w)
{
    struct stat st{};
    bool exists = (stat(w.path.c_str(), &st) == 0);
    time_t newMtime = exists ? st.st_mtime : -1;
    off_t  newSize  = exists ? st.st_size  : -1;

    // Fire on any of: appearance (size/mtime go from -1 to real),
    // disappearance (-> -1), content/size mtime change. Skip on
    // unchanged state (typical for a sibling-file write that woke us up
    // but didn't touch this file).
    if (newMtime == w.lastMtime && newSize == w.lastSize) return;
    w.lastMtime = newMtime;
    w.lastSize  = newSize;
    if (w.cb) w.cb();
}

void KQueueEventLoop::addFileWatch(const std::string& path, WatchCb cb)
{
    WatchEntry w;
    w.path = path;
    auto sep = w.path.rfind('/');
    if (sep != std::string::npos) {
        w.dir = w.path.substr(0, sep);
    } else {
        w.dir = ".";
    }
    if (w.dir.empty()) w.dir = ".";
    w.cb = std::move(cb);

    if (retainDirWatch(w.dir) < 0) {
        // Parent dir unopenable — drop the watch entirely.
        return;
    }

    // Seed lastMtime/lastSize from the current state so a no-op
    // recheckEntry doesn't fire on the first dir event after registration.
    struct stat st{};
    if (stat(w.path.c_str(), &st) == 0) {
        w.lastMtime = st.st_mtime;
        w.lastSize  = st.st_size;
    }
    fileWatches_.push_back(std::move(w));
}

void KQueueEventLoop::removeFileWatch()
{
    for (auto& w : fileWatches_) releaseDirWatch(w.dir);
    fileWatches_.clear();
    // Belt-and-braces: if anything went sideways and a DirWatch slipped
    // through, close them. Should be a no-op after the loop above.
    for (auto& [_, dw] : dirWatches_) {
        if (dw.fd >= 0) {
            struct kevent ev{};
            EV_SET(&ev, dw.fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
            kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
            close(dw.fd);
        }
    }
    dirWatches_.clear();
}
