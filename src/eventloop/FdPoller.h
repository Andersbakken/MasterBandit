#pragma once

#include <cstdint>
#include <functional>

// Cross-platform fd-readiness poller. Owned by a single dedicated polling
// thread (the thread that calls poll()). Mutators (add/update/disable/
// enable/remove) are thread-safe and may be called from any thread,
// including from inside a callback running on the polling thread.
//
// Backpressure note: callbacks run on the polling thread. If a callback
// fills a buffer and wants to disarm reads, it can call disable(fd) from
// inside the callback — the change applies on the same iteration without
// the wakeup-pipe roundtrip. enable(fd, …) called from a different thread
// queues a pending op + wakes the poller, which applies the change on the
// next iteration.
//
// HUP / EOF: the readable callback fires for HUP/EOF events too. The
// caller's callback is responsible for noticing the EOF (e.g. read(2)
// returning 0 or EIO) and removing the fd. FdPoller does not surface a
// separate HUP callback — keeping the interface minimal — and relies on
// the caller's read loop to detect end-of-file. This matches how
// Terminal::readFromFD already handles EOF/EIO via markExited().
class FdPoller
{
public:
    enum class Events : uint8_t {
        None      = 0,
        Readable  = 1,
        Writable  = 2,
        ReadWrite = 3
    };
    using FdCb = std::function<void(Events fired)>;

    virtual ~FdPoller() = default;

    // Register a new fd. cb fires from the polling thread when the fd
    // is ready for any of the requested events. Replaces any prior
    // registration for the same fd.
    virtual void add(int fd, Events events, FdCb cb) = 0;

    // Change the event mask for an already-registered fd. No-op if fd
    // is not registered.
    virtual void update(int fd, Events events) = 0;

    // Shorthands. disable == update(fd, None); enable == update(fd, evs).
    virtual void disable(int fd) = 0;
    virtual void enable(int fd, Events events) = 0;

    // Unregister fd. Asynchronous: queues the removal as a pending op,
    // which the polling thread applies on its next iteration. The
    // callback may still fire one more time after this returns.
    virtual void remove(int fd) = 0;

    // Synchronous unregister: blocks the calling thread until the
    // polling thread has applied the removal and any in-flight callback
    // for fd has returned. After this returns, no further callbacks
    // will fire for fd.
    //
    // MUST NOT be called from the polling thread (i.e. from inside a
    // callback) — that would deadlock.
    virtual void removeSync(int fd) = 0;

    // Block (up to timeoutMs, or indefinitely if -1) waiting for events.
    // Invokes registered callbacks for each fired fd. Returns the number
    // of fd events fired (excluding internal wakeups).
    //
    // Must be called from a single dedicated thread. Calling from
    // multiple threads concurrently is undefined.
    virtual int poll(int timeoutMs) = 0;

    // Wake the polling thread out of poll(). Thread-safe. Used
    // internally by mutators; also exposed so external code (e.g.
    // shutdown) can interrupt the poll without registering an op.
    virtual void wake() = 0;

    // Underlying OS fd (kqueue / epoll). Used by EventLoop
    // implementations that integrate FdPoller as a sub-poller of
    // their outer event loop: they add this fd to their own
    // kqueue/epoll/CFRunLoop and call poll(0) to drain when it
    // fires. Returns -1 on backends without an integratable fd.
    virtual int nativeHandle() const = 0;
};

inline FdPoller::Events operator|(FdPoller::Events a, FdPoller::Events b)
{
    return static_cast<FdPoller::Events>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool operator&(FdPoller::Events a, FdPoller::Events b)
{
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}
