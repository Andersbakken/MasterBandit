#include "EventLoop_kqueue.h"

#include <spdlog/spdlog.h>

#include <sys/event.h>
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
}

KQueueEventLoop::~KQueueEventLoop()
{
    if (watchFd_ >= 0) { close(watchFd_); watchFd_ = -1; }
    if (wakeupRead_  >= 0) { close(wakeupRead_);  wakeupRead_  = -1; }
    if (wakeupWrite_ >= 0) { close(wakeupWrite_); wakeupWrite_ = -1; }
    if (kqFd_ >= 0) { close(kqFd_); kqFd_ = -1; }
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

        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);

            if (events[i].filter == EVFILT_READ && fd == wakeupRead_) {
                // Drain the wakeup pipe
                char buf[64];
                while (read(wakeupRead_, buf, sizeof(buf)) > 0) {}
            } else if (events[i].filter == EVFILT_VNODE) {
                // File watch event
                if (fileWatchCb_) fileWatchCb_();
            } else if (events[i].filter == EVFILT_READ) {
                auto it = fds_.find(fd);
                if (it != fds_.end()) it->second.cb(FdEvents::Readable);
            } else if (events[i].filter == EVFILT_WRITE) {
                auto it = fds_.find(fd);
                if (it != fds_.end()) it->second.cb(FdEvents::Writable);
            }
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

// ---------- fd watching ----------

void KQueueEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    struct kevent evs[2];
    int n = 0;
    if (events & FdEvents::Readable)
        EV_SET(&evs[n++], fd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (events & FdEvents::Writable)
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (n) kevent(kqFd_, evs, n, nullptr, 0, nullptr);
    fds_[fd] = { events, std::move(cb) };
}

void KQueueEventLoop::updateFd(int fd, FdEvents events)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    FdEvents old = it->second.events;
    struct kevent evs[4];
    int n = 0;

    // Enable newly added filters, disable removed ones
    if ((events & FdEvents::Readable) && !(old & FdEvents::Readable))
        EV_SET(&evs[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    else if (!(events & FdEvents::Readable) && (old & FdEvents::Readable))
        EV_SET(&evs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);

    if ((events & FdEvents::Writable) && !(old & FdEvents::Writable))
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    else if (!(events & FdEvents::Writable) && (old & FdEvents::Writable))
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);

    if (n) kevent(kqFd_, evs, n, nullptr, 0, nullptr);
    it->second.events = events;
}

void KQueueEventLoop::removeFd(int fd)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    struct kevent evs[2];
    int n = 0;
    if (it->second.events & FdEvents::Readable)
        EV_SET(&evs[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    if (it->second.events & FdEvents::Writable)
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    if (n) kevent(kqFd_, evs, n, nullptr, 0, nullptr);
    fds_.erase(it);
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

// ---------- file watching ----------

void KQueueEventLoop::addFileWatch(const std::string& path, WatchCb cb)
{
    removeFileWatch();
    fileWatchPath_ = path;
    fileWatchCb_   = std::move(cb);

    watchFd_ = open(path.c_str(), O_RDONLY | O_EVTONLY);
    if (watchFd_ < 0) {
        spdlog::warn("KQueueEventLoop: addFileWatch open '{}': {}", path, strerror(errno));
        return;
    }

    struct kevent ev{};
    EV_SET(&ev, watchFd_, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB, 0, nullptr);
    kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
}

void KQueueEventLoop::removeFileWatch()
{
    if (watchFd_ >= 0) {
        struct kevent ev{};
        EV_SET(&ev, watchFd_, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
        kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
        close(watchFd_);
        watchFd_ = -1;
    }
    fileWatchPath_.clear();
    fileWatchCb_ = nullptr;
}
