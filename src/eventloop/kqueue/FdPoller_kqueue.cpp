#include "FdPoller_kqueue.h"

#include <spdlog/spdlog.h>

#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <future>
#include <stdexcept>

static constexpr int kMaxKEvents = 64;

FdPollerKQueue::FdPollerKQueue()
{
    kqFd_ = ::kqueue();
    if (kqFd_ < 0)
        throw std::runtime_error(std::string("FdPollerKQueue kqueue: ") + strerror(errno));

    int pipefd[2];
    if (::pipe(pipefd) < 0)
        throw std::runtime_error(std::string("FdPollerKQueue pipe: ") + strerror(errno));
    wakeRead_  = pipefd[0];
    wakeWrite_ = pipefd[1];
    fcntl(wakeRead_,  F_SETFL, O_NONBLOCK);
    fcntl(wakeWrite_, F_SETFL, O_NONBLOCK);

    struct kevent ev{};
    EV_SET(&ev, wakeRead_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(kqFd_, &ev, 1, nullptr, 0, nullptr);
}

FdPollerKQueue::~FdPollerKQueue()
{
    if (wakeRead_  >= 0) { ::close(wakeRead_);  wakeRead_  = -1; }
    if (wakeWrite_ >= 0) { ::close(wakeWrite_); wakeWrite_ = -1; }
    if (kqFd_      >= 0) { ::close(kqFd_);      kqFd_      = -1; }
}

void FdPollerKQueue::add(int fd, Events events, FdCb cb)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Add, fd, events, std::move(cb), {}});
    }
    writeWake();
}

void FdPollerKQueue::update(int fd, Events events)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Update, fd, events, {}, {}});
    }
    writeWake();
}

void FdPollerKQueue::disable(int fd)
{
    update(fd, Events::None);
}

void FdPollerKQueue::enable(int fd, Events events)
{
    update(fd, events);
}

void FdPollerKQueue::remove(int fd)
{
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingOps_.push_back({OpKind::Remove, fd, Events::None, {}, {}});
    }
    writeWake();
}

void FdPollerKQueue::removeSync(int fd)
{
    // Caller must not be the polling thread (would deadlock).
    if (pollThreadKnown_.load(std::memory_order_acquire)
        && pollThreadId_ == std::this_thread::get_id()) {
        spdlog::error("FdPollerKQueue::removeSync called from polling thread (fd={}) — would deadlock", fd);
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

void FdPollerKQueue::writeWake()
{
    char b = 1;
    ssize_t n = ::write(wakeWrite_, &b, 1);
    (void)n;
}

void FdPollerKQueue::wake()
{
    writeWake();
}

void FdPollerKQueue::drainWakePipe()
{
    char buf[64];
    while (::read(wakeRead_, buf, sizeof(buf)) > 0) {}
}

void FdPollerKQueue::drainPending()
{
    std::vector<PendingOp> local;
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        local.swap(pendingOps_);
    }
    for (auto& op : local) applyOp(op);
}

void FdPollerKQueue::applyOp(PendingOp& op)
{
    switch (op.kind) {
    case OpKind::Add: {
        // Replace any prior registration.
        auto it = fds_.find(op.fd);
        Events oldEvents = (it != fds_.end()) ? it->second.events : Events::None;

        // First, remove old kqueue subscriptions that won't be in the new mask.
        struct kevent evs[4];
        int n = 0;
        if ((oldEvents & Events::Readable) && !(op.events & Events::Readable))
            EV_SET(&evs[n++], op.fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        if ((oldEvents & Events::Writable) && !(op.events & Events::Writable))
            EV_SET(&evs[n++], op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        if ((op.events & Events::Readable) && !(oldEvents & Events::Readable))
            EV_SET(&evs[n++], op.fd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if ((op.events & Events::Writable) && !(oldEvents & Events::Writable))
            EV_SET(&evs[n++], op.fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (n) ::kevent(kqFd_, evs, n, nullptr, 0, nullptr);

        fds_[op.fd] = { op.events, std::move(op.cb) };
        break;
    }
    case OpKind::Update: {
        auto it = fds_.find(op.fd);
        if (it == fds_.end()) return;
        Events oldEvents = it->second.events;
        if (oldEvents == op.events) return;

        struct kevent evs[4];
        int n = 0;
        if ((op.events & Events::Readable) && !(oldEvents & Events::Readable))
            EV_SET(&evs[n++], op.fd, EVFILT_READ,  EV_ADD | EV_ENABLE, 0, 0, nullptr);
        else if (!(op.events & Events::Readable) && (oldEvents & Events::Readable))
            EV_SET(&evs[n++], op.fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);

        if ((op.events & Events::Writable) && !(oldEvents & Events::Writable))
            EV_SET(&evs[n++], op.fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        else if (!(op.events & Events::Writable) && (oldEvents & Events::Writable))
            EV_SET(&evs[n++], op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);

        if (n) ::kevent(kqFd_, evs, n, nullptr, 0, nullptr);
        it->second.events = op.events;
        break;
    }
    case OpKind::Remove: {
        auto it = fds_.find(op.fd);
        if (it == fds_.end()) return;
        struct kevent evs[2];
        int n = 0;
        if (it->second.events & Events::Readable)
            EV_SET(&evs[n++], op.fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        if (it->second.events & Events::Writable)
            EV_SET(&evs[n++], op.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        if (n) ::kevent(kqFd_, evs, n, nullptr, 0, nullptr);
        fds_.erase(it);
        break;
    }
    case OpKind::RemoveAck:
        if (op.ack) op.ack();
        break;
    }
}

int FdPollerKQueue::poll(int timeoutMs)
{
    if (!pollThreadKnown_.load(std::memory_order_acquire)) {
        pollThreadId_ = std::this_thread::get_id();
        pollThreadKnown_.store(true, std::memory_order_release);
    }

    // Apply any ops that arrived while we were elsewhere.
    drainPending();

    struct kevent evs[kMaxKEvents];
    struct timespec ts;
    struct timespec* tsPtr = nullptr;
    if (timeoutMs >= 0) {
        ts.tv_sec  = timeoutMs / 1000;
        ts.tv_nsec = static_cast<long>((timeoutMs % 1000) * 1'000'000);
        tsPtr = &ts;
    }

    int n = ::kevent(kqFd_, nullptr, 0, evs, kMaxKEvents, tsPtr);
    if (n < 0) {
        if (errno == EINTR) return 0;
        spdlog::error("FdPollerKQueue: kevent: {}", strerror(errno));
        return 0;
    }

    int fired = 0;
    bool wakeFired = false;
    for (int i = 0; i < n; ++i) {
        int fd = static_cast<int>(evs[i].ident);
        if (fd == wakeRead_ && evs[i].filter == EVFILT_READ) {
            wakeFired = true;
            continue;
        }
        Events e = Events::None;
        if (evs[i].filter == EVFILT_READ)  e = Events::Readable;
        else if (evs[i].filter == EVFILT_WRITE) e = Events::Writable;

        auto it = fds_.find(fd);
        if (it == fds_.end()) continue;  // stale event for an fd we just removed
        // Capture the callback under a copy so the callback may safely
        // call back into FdPoller (e.g. disable(fd)) which appends to
        // pendingOps_ — that doesn't touch fds_, so we're fine. But the
        // callback could also remove(fd) via removeSync from another
        // thread; in that case fds_ is still intact for this iteration.
        FdCb cb = it->second.cb;
        cb(e);
        ++fired;
    }

    if (wakeFired) {
        drainWakePipe();
        // Apply any ops queued while we were processing — drainPending
        // at top-of-loop catches the next iteration, but if a callback
        // queued an op we want to apply it before sleeping again. The
        // top-of-loop drain on the next iteration also handles this; do
        // it here too so a single-shot poll() (timeoutMs==0 in tests)
        // still picks up callback-queued ops.
        drainPending();
    }

    return fired;
}
