#pragma once

#include <eventloop/FdPoller.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

// Dedicated PTY-read multiplexer. Owns one FdPoller and one polling
// thread. PTY master fds are registered for Readable events; the
// associated callback runs on the polling thread (NOT main) when the
// kernel signals data is ready, and is responsible for reading from
// the fd into a coalesce buffer that a worker thread later drains.
//
// Backpressure: the read callback calls disable(fd) from inside itself
// to disarm POLLIN once its buffer hits high-water; the worker calls
// enable(fd, Readable) once it has drained below low-water. The
// disable side is in-thread (no wakeup roundtrip); the enable side
// queues a pending op and wakes the poller.
//
// Lifetime: PlatformDawn owns one PtyMux. It is started after the
// EventLoop is constructed and stopped before the EventLoop is
// destroyed. Terminals register/unregister their master fd through
// add/remove; remove is synchronous so the caller can safely free
// captured state (the Terminal pointer) after it returns.
class PtyMux
{
public:
    using OnReadable = std::function<void()>;

    PtyMux();
    ~PtyMux();

    // Spawn the polling thread. Idempotent.
    void start();

    // Signal the thread to exit and join. Safe to call multiple times.
    // After stop() returns, no callbacks will fire.
    void stop();

    // Register fd. cb fires on the PtyMux thread when fd is readable
    // (or has hit EOF/HUP — the cb's read(2) will return 0/EIO and
    // should call its terminal's markExited()).
    //
    // Asynchronous; the registration takes effect on the next mux
    // iteration. Safe to call from any thread.
    void add(int fd, OnReadable onReadable);

    // Synchronous removal. Blocks until the mux thread has applied
    // the removal and any in-flight callback for fd has returned.
    // After this returns, no further callbacks will fire for fd.
    //
    // MUST NOT be called from inside an OnReadable callback (would
    // deadlock — the caller IS the mux thread). Use removeAsync for
    // self-removal from a callback.
    void remove(int fd);

    // Async removal. The mux thread processes the remove on its next
    // iteration. The captures in the previously-registered callback
    // must remain valid until then. Use this from inside a callback,
    // or whenever the caller can guarantee captured state outlives
    // the mux thread's next iteration.
    void removeAsync(int fd);

    // Backpressure. disable() is in-thread when called from a
    // callback; otherwise it queues a pending op like enable() does.
    void disable(int fd);
    void enable(int fd);

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    void threadMain();

    std::unique_ptr<FdPoller> poller_;
    std::thread               thread_;
    std::atomic<bool>         running_       { false };
    std::atomic<bool>         stopRequested_ { false };
};
