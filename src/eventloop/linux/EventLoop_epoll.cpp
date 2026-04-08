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

int EpollEventLoop::epollEventsFor(FdEvents ev)
{
    int flags = 0;
    if (ev & FdEvents::Readable) flags |= EPOLLIN | EPOLLHUP;
    if (ev & FdEvents::Writable) flags |= EPOLLOUT;
    return flags;
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
}

EpollEventLoop::~EpollEventLoop()
{
    if (inotifyFd_ >= 0) close(inotifyFd_);
    if (timerFd_  >= 0) close(timerFd_);
    if (wakeupFd_ >= 0) close(wakeupFd_);
    if (epollFd_  >= 0) close(epollFd_);
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

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == wakeupFd_) {
                drainWakeup();
            } else if (fd == timerFd_) {
                // Drain the timerfd read
                uint64_t expirations;
                read(timerFd_, &expirations, sizeof(expirations));
                drainTimers();
            } else if (fd == inotifyFd_) {
                drainInotify();
            } else {
                auto it = fds_.find(fd);
                if (it != fds_.end()) {
                    FdEvents fired = static_cast<FdEvents>(0);
                    if (events[i].events & (EPOLLIN | EPOLLHUP))  fired = fired | FdEvents::Readable;
                    if (events[i].events & EPOLLOUT) fired = fired | FdEvents::Writable;
                    if (static_cast<uint8_t>(fired))
                        it->second.cb(fired);
                }
            }
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
    write(wakeupFd_, &one, sizeof(one));
}

void EpollEventLoop::drainWakeup()
{
    uint64_t val;
    read(wakeupFd_, &val, sizeof(val));
}

// ---------- fd watching ----------

void EpollEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    epoll_event ev{};
    ev.events = epollEventsFor(events);
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        spdlog::error("EpollEventLoop: watchFd EPOLL_CTL_ADD fd={}: {}", fd, strerror(errno));
        return;
    }
    fds_[fd] = { events, std::move(cb) };
}

void EpollEventLoop::updateFd(int fd, FdEvents events)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    epoll_event ev{};
    ev.events = epollEventsFor(events);
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        spdlog::error("EpollEventLoop: updateFd EPOLL_CTL_MOD fd={}: {}", fd, strerror(errno));
        return;
    }
    it->second.events = events;
}

void EpollEventLoop::removeFd(int fd)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    fds_.erase(it);
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
    removeFileWatch();
    // Watch the parent directory so renames (atomic saves) are detected
    std::string dir = path;
    auto sep = dir.rfind('/');
    if (sep != std::string::npos) dir.resize(sep);
    if (dir.empty()) dir = ".";

    inotifyWd_ = inotify_add_watch(inotifyFd_, dir.c_str(),
                                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (inotifyWd_ < 0) {
        spdlog::warn("EpollEventLoop: inotify_add_watch '{}': {}", dir, strerror(errno));
        return;
    }
    fileWatchPath_ = path;
    fileWatchCb_   = std::move(cb);
}

void EpollEventLoop::removeFileWatch()
{
    if (inotifyWd_ >= 0) {
        inotify_rm_watch(inotifyFd_, inotifyWd_);
        inotifyWd_ = -1;
    }
    fileWatchPath_.clear();
    fileWatchCb_ = nullptr;
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

        // Check if any event matches the watched file (by name)
        std::string filename;
        if (!fileWatchPath_.empty()) {
            auto sep = fileWatchPath_.rfind('/');
            filename = (sep != std::string::npos)
                     ? fileWatchPath_.substr(sep + 1)
                     : fileWatchPath_;
        }

        const char* p = buf;
        while (p < buf + len) {
            const auto* event = reinterpret_cast<const inotify_event*>(p);
            if (fileWatchCb_ && event->len > 0 && filename == event->name)
                fileWatchCb_();
            p += sizeof(inotify_event) + event->len;
        }
    }
}
