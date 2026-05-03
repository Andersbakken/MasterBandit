#include "FdPoller_epoll.h"

#include <spdlog/spdlog.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <future>
#include <stdexcept>

static constexpr int kMaxEpollEvents = 64;

static int epollFlagsFor(FdPoller::Events ev)
{
    int flags = 0;
    if (ev & FdPoller::Events::Readable) flags |= EPOLLIN | EPOLLHUP | EPOLLRDHUP;
    if (ev & FdPoller::Events::Writable) flags |= EPOLLOUT;
    return flags;
}

FdPollerEpoll::FdPollerEpoll()
{
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0)
        throw std::runtime_error(std::string("FdPollerEpoll epoll_create1: ") + strerror(errno));

    wakeupFd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeupFd_ < 0)
        throw std::runtime_error(std::string("FdPollerEpoll eventfd: ") + strerror(errno));

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeupFd_;
    ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev);
}

FdPollerEpoll::~FdPollerEpoll()
{
    if (wakeupFd_ >= 0) { ::close(wakeupFd_); wakeupFd_ = -1; }
    if (epollFd_  >= 0) { ::close(epollFd_);  epollFd_  = -1; }
}

void FdPollerEpoll::add(int fd, Events events, FdCb cb)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Add, fd, events, std::move(cb), {}});
    }
    writeWake();
}

void FdPollerEpoll::update(int fd, Events events)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Update, fd, events, {}, {}});
    }
    writeWake();
}

void FdPollerEpoll::disable(int fd)
{
    update(fd, Events::None);
}

void FdPollerEpoll::enable(int fd, Events events)
{
    update(fd, events);
}

void FdPollerEpoll::remove(int fd)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Remove, fd, Events::None, {}, {}});
    }
    writeWake();
}

void FdPollerEpoll::removeSync(int fd)
{
    if (pollThreadKnown_.load(std::memory_order_acquire)
        && pollThreadId_ == std::this_thread::get_id()) {
        spdlog::error("FdPollerEpoll::removeSync called from polling thread (fd={}) — would deadlock", fd);
        return;
    }

    std::promise<void> done;
    auto fut = done.get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Remove, fd, Events::None, {}, {}});
        pendingOps_.push_back({OpKind::RemoveAck, -1, Events::None, {},
                               [&done]() { done.set_value(); }});
    }
    writeWake();
    fut.wait();
}

void FdPollerEpoll::writeWake()
{
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void FdPollerEpoll::wake()
{
    writeWake();
}

void FdPollerEpoll::drainWakeFd()
{
    uint64_t val;
    ssize_t n = ::read(wakeupFd_, &val, sizeof(val));
    (void)n;
}

void FdPollerEpoll::drainPending()
{
    std::vector<PendingOp> local;
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        local.swap(pendingOps_);
    }
    for (auto& op : local) applyOp(op);
}

void FdPollerEpoll::applyOp(PendingOp& op)
{
    switch (op.kind) {
    case OpKind::Add: {
        auto it = fds_.find(op.fd);
        epoll_event ev{};
        ev.events = epollFlagsFor(op.events);
        ev.data.fd = op.fd;
        if (it == fds_.end()) {
            if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, op.fd, &ev) < 0) {
                spdlog::error("FdPollerEpoll::add EPOLL_CTL_ADD fd={}: {}", op.fd, strerror(errno));
                return;
            }
        } else {
            // Replace: keep the kernel-side registration, just update mask + cb.
            if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, op.fd, &ev) < 0) {
                spdlog::error("FdPollerEpoll::add (replace) EPOLL_CTL_MOD fd={}: {}", op.fd, strerror(errno));
                return;
            }
        }
        fds_[op.fd] = { op.events, std::move(op.cb) };
        break;
    }
    case OpKind::Update: {
        auto it = fds_.find(op.fd);
        if (it == fds_.end()) return;
        if (it->second.events == op.events) return;
        epoll_event ev{};
        ev.events = epollFlagsFor(op.events);
        ev.data.fd = op.fd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, op.fd, &ev) < 0) {
            spdlog::error("FdPollerEpoll::update EPOLL_CTL_MOD fd={}: {}", op.fd, strerror(errno));
            return;
        }
        it->second.events = op.events;
        break;
    }
    case OpKind::Remove: {
        auto it = fds_.find(op.fd);
        if (it == fds_.end()) return;
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, op.fd, nullptr);
        fds_.erase(it);
        break;
    }
    case OpKind::RemoveAck:
        if (op.ack) op.ack();
        break;
    }
}

int FdPollerEpoll::poll(int timeoutMs)
{
    if (!pollThreadKnown_.load(std::memory_order_acquire)) {
        pollThreadId_ = std::this_thread::get_id();
        pollThreadKnown_.store(true, std::memory_order_release);
    }

    drainPending();

    epoll_event evs[kMaxEpollEvents];
    int n = ::epoll_wait(epollFd_, evs, kMaxEpollEvents, timeoutMs);
    if (n < 0) {
        if (errno == EINTR) return 0;
        spdlog::error("FdPollerEpoll: epoll_wait: {}", strerror(errno));
        return 0;
    }

    int fired = 0;
    bool wakeFired = false;
    for (int i = 0; i < n; ++i) {
        int fd = evs[i].data.fd;
        if (fd == wakeupFd_) {
            wakeFired = true;
            continue;
        }
        Events e = Events::None;
        if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
            e = e | Events::Readable;
        if (evs[i].events & EPOLLOUT)
            e = e | Events::Writable;
        if (!static_cast<uint8_t>(e)) continue;

        auto it = fds_.find(fd);
        if (it == fds_.end()) continue;
        FdCb cb = it->second.cb;
        cb(e);
        ++fired;
    }

    if (wakeFired) {
        drainWakeFd();
        drainPending();
    }

    return fired;
}
