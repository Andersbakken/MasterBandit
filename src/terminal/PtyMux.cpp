#include "PtyMux.h"

#if defined(__APPLE__)
#  include <eventloop/kqueue/FdPoller_kqueue.h>
#else
#  include <eventloop/epoll/FdPoller_epoll.h>
#endif

#include <spdlog/spdlog.h>

PtyMux::PtyMux()
{
#if defined(__APPLE__)
    poller_ = std::make_unique<FdPollerKQueue>();
#else
    poller_ = std::make_unique<FdPollerEpoll>();
#endif
}

PtyMux::~PtyMux()
{
    stop();
}

void PtyMux::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread([this]() { threadMain(); });
}

void PtyMux::stop()
{
    if (!running_.load(std::memory_order_acquire)) return;
    stopRequested_.store(true, std::memory_order_release);
    poller_->wake();
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void PtyMux::add(int fd, OnReadable onReadable)
{
    if (!running_.load(std::memory_order_acquire)) return;
    auto cb = [onReadable = std::move(onReadable)](FdPoller::Events) {
        onReadable();
    };
    poller_->add(fd, FdPoller::Events::Readable, std::move(cb));
}

void PtyMux::remove(int fd)
{
    // After stop() the polling thread is gone — removeSync would
    // deadlock on a future the polling thread will never signal.
    // No subscriptions exist past stop() anyway (the poller will be
    // destroyed shortly), so a no-op is safe.
    if (!running_.load(std::memory_order_acquire)) return;
    poller_->removeSync(fd);
}

void PtyMux::removeAsync(int fd)
{
    if (!running_.load(std::memory_order_acquire)) return;
    poller_->remove(fd);
}

void PtyMux::disable(int fd)
{
    if (!running_.load(std::memory_order_acquire)) return;
    poller_->disable(fd);
}

void PtyMux::enable(int fd)
{
    if (!running_.load(std::memory_order_acquire)) return;
    poller_->enable(fd, FdPoller::Events::Readable);
}

void PtyMux::threadMain()
{
    while (!stopRequested_.load(std::memory_order_acquire)) {
        poller_->poll(-1);
    }
}
