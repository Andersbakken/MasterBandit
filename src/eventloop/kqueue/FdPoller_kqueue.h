#pragma once

#include "../FdPoller.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class FdPollerKQueue final : public FdPoller
{
public:
    FdPollerKQueue();
    ~FdPollerKQueue() override;

    void add(int fd, Events events, FdCb cb) override;
    void update(int fd, Events events) override;
    void disable(int fd) override;
    void enable(int fd, Events events) override;
    void remove(int fd) override;
    void removeSync(int fd) override;
    int  poll(int timeoutMs) override;
    void wake() override;
    int  nativeHandle() const override { return kqFd_; }

private:
    enum class OpKind { Add, Update, Remove, RemoveAck };

    struct PendingOp {
        OpKind kind;
        int    fd;
        Events events;
        FdCb   cb;
        // For RemoveAck: callable invoked from the polling thread after
        // the corresponding Remove has been applied. Used by removeSync
        // as a fence.
        std::function<void()> ack;
    };

    struct FdEntry {
        Events events;
        FdCb   cb;
    };

    void drainPending();
    void applyOp(PendingOp& op);
    void writeWake();
    void drainWakePipe();

    int kqFd_       = -1;
    int wakeRead_   = -1;
    int wakeWrite_  = -1;

    std::mutex             pendingMu_;
    std::vector<PendingOp> pendingOps_;

    // Touched only by the polling thread.
    std::unordered_map<int, FdEntry> fds_;

    // Set on the first poll() call, then asserted on subsequent ones.
    // Debug-only; no enforcement in release.
    std::atomic<bool>      pollThreadKnown_ { false };
    std::thread::id        pollThreadId_;
};
