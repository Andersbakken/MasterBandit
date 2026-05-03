#pragma once

#include "../FdPoller.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class FdPollerEpoll final : public FdPoller
{
public:
    FdPollerEpoll();
    ~FdPollerEpoll() override;

    void add(int fd, Events events, FdCb cb) override;
    void update(int fd, Events events) override;
    void disable(int fd) override;
    void enable(int fd, Events events) override;
    void remove(int fd) override;
    void removeSync(int fd) override;
    int  poll(int timeoutMs) override;
    void wake() override;
    int  nativeHandle() const override { return epollFd_; }

private:
    enum class OpKind { Add, Update, Remove, RemoveAck };

    struct PendingOp {
        OpKind kind;
        int    fd;
        Events events;
        FdCb   cb;
        std::function<void()> ack;
    };

    struct FdEntry {
        Events events;
        FdCb   cb;
    };

    void drainPending();
    void applyOp(PendingOp& op);
    void writeWake();
    void drainWakeFd();

    int epollFd_  = -1;
    int wakeupFd_ = -1;  // eventfd

    std::mutex             pendingMu_;
    std::vector<PendingOp> pendingOps_;

    // Touched only by the polling thread.
    std::unordered_map<int, FdEntry> fds_;

    std::atomic<bool>      pollThreadKnown_ { false };
    std::thread::id        pollThreadId_;
};
