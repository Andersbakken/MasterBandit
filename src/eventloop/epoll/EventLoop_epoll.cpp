#include "EventLoop_epoll.h"

#include <spdlog/spdlog.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

// ---------- helpers ----------

static constexpr int MaxEvents = 64;

uint64_t EpollEventLoop::nowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------- lifecycle ----------

EpollEventLoop::EpollEventLoop()
{
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0)
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));

    // Wakeup fd
    wakeupFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeupFd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeupFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev);

    // Timer fd (disarmed initially)
    timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timerFd_ < 0)
        throw std::runtime_error(std::string("timerfd_create: ") + strerror(errno));

    ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = timerFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerFd_, &ev);

    // Inotify fd (created now, watches added on addFileWatch)
    inotifyFd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotifyFd_ < 0)
        throw std::runtime_error(std::string("inotify_init1: ") + strerror(errno));

    ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = inotifyFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, inotifyFd_, &ev);

    // Per-fd watching delegates to FdPoller. Add its epoll fd to
    // our outer epoll so any managed fd's readiness wakes us; we
    // then drain the poller via poll(0).
    fdPoller_ = std::make_unique<FdPollerEpoll>();
    innerPollerFd_ = fdPoller_->nativeHandle();
    if (innerPollerFd_ >= 0) {
        ev = {};
        ev.events = EPOLLIN;
        ev.data.fd = innerPollerFd_;
        epoll_ctl(epollFd_, EPOLL_CTL_ADD, innerPollerFd_, &ev);
    }
}

EpollEventLoop::~EpollEventLoop()
{
    if (innerPollerFd_ >= 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, innerPollerFd_, nullptr);
        innerPollerFd_ = -1;
    }
    fdPoller_.reset();
    if (inotifyFd_ >= 0) close(inotifyFd_);
    if (timerFd_  >= 0) close(timerFd_);
    if (wakeupFd_ >= 0) close(wakeupFd_);
    if (epollFd_  >= 0) close(epollFd_);
}

FdPoller::Events EpollEventLoop::toPoller(FdEvents ev)
{
    return static_cast<FdPoller::Events>(static_cast<uint8_t>(ev));
}

EventLoop::FdEvents EpollEventLoop::fromPoller(FdPoller::Events ev)
{
    return static_cast<FdEvents>(static_cast<uint8_t>(ev));
}

// ---------- run / stop / wakeup ----------

void EpollEventLoop::run()
{
    running_ = true;
    epoll_event events[MaxEvents];

    while (running_) {
        int n = epoll_wait(epollFd_, events, MaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("EpollEventLoop: epoll_wait: {}", strerror(errno));
            break;
        }

        bool drainPoller = false;

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == wakeupFd_) {
                drainWakeup();
            } else if (fd == timerFd_) {
                // Drain the timerfd read
                uint64_t expirations;
                [[maybe_unused]] auto n = read(timerFd_, &expirations, sizeof(expirations));
                drainTimers();
            } else if (fd == inotifyFd_) {
                drainInotify();
            } else if (fd == innerPollerFd_) {
                drainPoller = true;
            }
            // Per-fd events live on the inner FdPoller now; nothing
            // else should reach this branch.
        }

        if (drainPoller && fdPoller_) {
            fdPoller_->poll(0);
        }

        if (onTick) onTick();
    }
}

void EpollEventLoop::stop()
{
    running_ = false;
    wakeup();
}

void EpollEventLoop::wakeup()
{
    uint64_t one = 1;
    [[maybe_unused]] auto n = write(wakeupFd_, &one, sizeof(one));
}

void EpollEventLoop::drainWakeup()
{
    uint64_t val;
    [[maybe_unused]] auto n = read(wakeupFd_, &val, sizeof(val));
}

// ---------- fd watching (delegated to FdPoller) ----------

void EpollEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    if (!fdPoller_) return;
    auto inner = std::move(cb);
    fdPoller_->add(fd, toPoller(events),
        [inner = std::move(inner)](FdPoller::Events ev) {
            if (inner) inner(fromPoller(ev));
        });
}

void EpollEventLoop::updateFd(int fd, FdEvents events)
{
    if (!fdPoller_) return;
    fdPoller_->update(fd, toPoller(events));
}

void EpollEventLoop::removeFd(int fd)
{
    if (!fdPoller_) return;
    fdPoller_->remove(fd);
}

// ---------- timers ----------

void EpollEventLoop::updateTimerFd()
{
    if (timers_.empty()) {
        // Disarm
        itimerspec spec{};
        timerfd_settime(timerFd_, TFD_TIMER_ABSTIME, &spec, nullptr);
        return;
    }

    uint64_t nextNs = timers_.top().nextFireNs;
    itimerspec spec{};
    spec.it_value.tv_sec  = static_cast<time_t>(nextNs / 1'000'000'000ULL);
    spec.it_value.tv_nsec = static_cast<long>(nextNs % 1'000'000'000ULL);
    // it_interval left zero — we rearm manually after each fire
    timerfd_settime(timerFd_, TFD_TIMER_ABSTIME, &spec, nullptr);
}

EpollEventLoop::TimerId EpollEventLoop::addTimer(uint64_t ms, bool repeat, TimerCb cb)
{
    TimerId id = nextTimerId_++;
    uint64_t fireNs = nowNs() + ms * 1'000'000ULL;
    timers_.push({ id, ms, repeat, std::move(cb), fireNs });
    updateTimerFd();
    return id;
}

void EpollEventLoop::removeTimer(TimerId id)
{
    // Rebuild the heap without the matching timer.
    // std::priority_queue doesn't support O(1) removal; for the small number
    // of timers used here this is fine.
    std::vector<Timer> remaining;
    remaining.reserve(timers_.size());
    while (!timers_.empty()) {
        Timer t = timers_.top(); timers_.pop();
        if (t.id != id) remaining.push_back(std::move(t));
    }
    for (auto& t : remaining) timers_.push(std::move(t));
    updateTimerFd();
}

void EpollEventLoop::restartTimer(TimerId id)
{
    uint64_t now = nowNs();
    std::vector<Timer> remaining;
    remaining.reserve(timers_.size());
    while (!timers_.empty()) {
        Timer t = timers_.top(); timers_.pop();
        if (t.id == id) {
            t.nextFireNs = now + t.ms * 1'000'000ULL;
        }
        remaining.push_back(std::move(t));
    }
    for (auto& t : remaining) timers_.push(std::move(t));
    updateTimerFd();
}

void EpollEventLoop::drainTimers()
{
    uint64_t now = nowNs();

    // Fire all timers whose deadline has passed
    while (!timers_.empty() && timers_.top().nextFireNs <= now) {
        Timer t = timers_.top(); timers_.pop();
        t.cb();
        if (t.repeat) {
            t.nextFireNs = nowNs() + t.ms * 1'000'000ULL;
            timers_.push(std::move(t));
        }
    }

    updateTimerFd();
}

// ---------- file watching ----------

void EpollEventLoop::addFileWatch(const std::string& path, WatchCb cb)
{
    // Watch the parent directory so renames (atomic saves) are detected.
    std::string dir = path;
    std::string name;
    auto sep = dir.rfind('/');
    if (sep != std::string::npos) {
        name = dir.substr(sep + 1);
        dir.resize(sep);
    } else {
        name = dir;
        dir = ".";
    }
    if (dir.empty()) dir = ".";

    // If a watch already exists for a different parent dir, drop it. New
    // dir replaces old. Multi-dir support would need one inotify_add_watch
    // per dir; not required by current callers.
    if (!fileWatches_.empty() && dir != inotifyDir_) {
        removeFileWatch();
    }

    if (inotifyWd_ < 0) {
        inotifyWd_ = inotify_add_watch(inotifyFd_, dir.c_str(),
                                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (inotifyWd_ < 0) {
            spdlog::warn("EpollEventLoop: inotify_add_watch '{}': {}", dir, strerror(errno));
            return;
        }
        inotifyDir_ = dir;
    }
    fileWatches_.push_back({std::move(name), std::move(cb)});
}

void EpollEventLoop::removeFileWatch()
{
    if (inotifyWd_ >= 0) {
        inotify_rm_watch(inotifyFd_, inotifyWd_);
        inotifyWd_ = -1;
    }
    inotifyDir_.clear();
    fileWatches_.clear();
}

void EpollEventLoop::drainInotify()
{
    char buf[4096] __attribute__((aligned(__alignof__(inotify_event))));
    for (;;) {
        ssize_t len = read(inotifyFd_, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            spdlog::error("EpollEventLoop: inotify read: {}", strerror(errno));
            break;
        }

        const char* p = buf;
        while (p < buf + len) {
            const auto* event = reinterpret_cast<const inotify_event*>(p);
            if (event->len > 0) {
                // Fire any callbacks whose filename matches this event.
                // Iterate by index so callbacks that mutate fileWatches_
                // (e.g. via removeFileWatch) don't invalidate our iterator.
                for (size_t i = 0; i < fileWatches_.size(); ++i) {
                    if (fileWatches_[i].name == event->name) {
                        WatchCb cb = fileWatches_[i].cb;
                        if (cb) cb();
                        // The vector may have been mutated; rescan from
                        // the same index. Different name matches in the
                        // remaining slots still get called below.
                    }
                }
            }
            p += sizeof(inotify_event) + event->len;
        }
    }
}
