#include <doctest/doctest.h>

#include "PtyMux.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// Helper: create a pipe and set both ends non-blocking. The read end
// gets handed to PtyMux; the write end is the producer.
struct Pipe
{
    int r = -1, w = -1;
    Pipe()
    {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        r = fds[0];
        w = fds[1];
        ::fcntl(r, F_SETFL, O_NONBLOCK);
        ::fcntl(w, F_SETFL, O_NONBLOCK);
    }
    ~Pipe()
    {
        if (r >= 0) ::close(r);
        if (w >= 0) ::close(w);
    }
};

// Spin-wait up to `timeoutMs` for `pred` to return true. Returns true
// if it did, false on timeout.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 1000)
{
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("PtyMux: basic add/remove and read callback fires")
{
    PtyMux mux;
    mux.start();
    CHECK(mux.isRunning());

    Pipe p;
    std::atomic<int> reads{0};
    std::atomic<size_t> bytes{0};

    mux.add(p.r, [&]() {
        char buf[256];
        for (;;) {
            int n;
            do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
            if (n <= 0) return;
            bytes.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const char* msg = "hello, world";
    REQUIRE(::write(p.w, msg, ::strlen(msg)) == static_cast<ssize_t>(::strlen(msg)));

    REQUIRE(waitFor([&]() { return bytes.load() == ::strlen(msg); }));
    CHECK(reads.load() >= 1);

    mux.remove(p.r);

    // After remove, further writes must NOT fire the callback.
    int readsBefore = reads.load();
    REQUIRE(::write(p.w, "more", 4) == 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(reads.load() == readsBefore);

    mux.stop();
}

TEST_CASE("PtyMux: backpressure cycle (disable then enable)")
{
    PtyMux mux;
    mux.start();

    Pipe p;
    std::atomic<int> reads{0};
    std::atomic<bool> paused{false};

    // Callback that disables itself on first read, simulating
    // Terminal::readFromFD hitting high-water.
    mux.add(p.r, [&]() {
        char buf[1024];
        for (;;) {
            int n;
            do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
            if (n <= 0) break;
        }
        reads.fetch_add(1, std::memory_order_relaxed);
        if (!paused.load()) {
            paused.store(true);
            mux.disable(p.r);
        }
    });

    REQUIRE(::write(p.w, "first", 5) == 5);
    REQUIRE(waitFor([&]() { return reads.load() >= 1; }));

    int readsAtPause = reads.load();

    // While paused, more writes should not fire the callback.
    REQUIRE(::write(p.w, "while-paused", 12) == 12);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(reads.load() == readsAtPause);

    // Re-enable from a different thread (simulates the parse worker
    // calling maybeResumeRead). Pending bytes from above should now
    // fire the callback.
    paused.store(false);
    std::thread enabler([&]() { mux.enable(p.r); });
    enabler.join();

    REQUIRE(waitFor([&]() { return reads.load() > readsAtPause; }));
    CHECK(reads.load() > readsAtPause);

    mux.remove(p.r);
    mux.stop();
}

TEST_CASE("PtyMux: EOF on pipe close fires callback exactly once")
{
    PtyMux mux;
    mux.start();

    Pipe p;
    std::atomic<int> eofCount{0};
    std::atomic<int> totalCalls{0};

    mux.add(p.r, [&]() {
        totalCalls.fetch_add(1, std::memory_order_relaxed);
        char buf[64];
        int n;
        do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
        if (n == 0) {
            eofCount.fetch_add(1, std::memory_order_relaxed);
            // Mirror Terminal::markExited's pattern: drop the
            // subscription from inside the callback (async — sync
            // would deadlock).
            mux.removeAsync(p.r);
        }
    });

    // Close the write end. The reader sees EOF.
    ::close(p.w);
    p.w = -1;

    REQUIRE(waitFor([&]() { return eofCount.load() == 1; }));
    // Give the mux a moment to confirm no spurious re-fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(eofCount.load() == 1);

    mux.stop();
}

TEST_CASE("PtyMux: concurrent add/remove from multiple threads")
{
    PtyMux mux;
    mux.start();

    constexpr int kThreads = 4;
    constexpr int kIters   = 25;

    std::vector<std::thread> workers;
    std::atomic<int> errors{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i) {
                Pipe p;
                std::atomic<int> hits{0};
                mux.add(p.r, [&]() {
                    char buf[64];
                    int n;
                    do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
                    (void)n;
                    hits.fetch_add(1, std::memory_order_relaxed);
                });
                if (::write(p.w, "x", 1) != 1) {
                    errors.fetch_add(1);
                    continue;
                }
                if (!waitFor([&]() { return hits.load() >= 1; }, 500)) {
                    errors.fetch_add(1);
                }
                mux.remove(p.r);
            }
        });
    }
    for (auto& th : workers) th.join();

    CHECK(errors.load() == 0);
    mux.stop();
}

TEST_CASE("PtyMux: sync remove blocks until in-flight callback returns")
{
    PtyMux mux;
    mux.start();

    Pipe p;
    std::atomic<bool> inCallback{false};
    std::atomic<bool> proceed{false};
    std::atomic<int> calls{0};

    mux.add(p.r, [&]() {
        char buf[64];
        int n;
        do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
        (void)n;
        inCallback.store(true);
        // Pretend to do slow work; release once the test signals.
        while (!proceed.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        calls.fetch_add(1);
        inCallback.store(false);
    });

    REQUIRE(::write(p.w, "x", 1) == 1);
    REQUIRE(waitFor([&]() { return inCallback.load(); }));

    // Issue the sync remove from another thread; it must block
    // because the callback is still running.
    std::atomic<bool> removeReturned{false};
    std::thread rm([&]() {
        mux.remove(p.r);
        removeReturned.store(true);
    });

    // Brief wait — remove should still be blocked.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(removeReturned.load());

    // Release the callback; remove should now return.
    proceed.store(true, std::memory_order_release);
    rm.join();
    CHECK(removeReturned.load());
    CHECK(calls.load() == 1);

    mux.stop();
}

TEST_CASE("PtyMux: stop() with registered fds is clean")
{
    PtyMux mux;
    mux.start();

    std::vector<Pipe> pipes(5);
    std::atomic<int> reads{0};
    for (auto& p : pipes) {
        mux.add(p.r, [&p, &reads]() {
            char buf[64];
            int n;
            do { n = ::read(p.r, buf, sizeof(buf)); } while (n == -1 && errno == EINTR);
            (void)n;
            reads.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // Hit one of them so we know the mux is processing.
    REQUIRE(::write(pipes[0].w, "x", 1) == 1);
    REQUIRE(waitFor([&]() { return reads.load() >= 1; }));

    // Don't remove — stop directly. Should not deadlock or leak.
    mux.stop();
    CHECK_FALSE(mux.isRunning());

    // Post-stop API should be safe no-ops.
    mux.remove(pipes[0].r);
    mux.disable(pipes[0].r);
    mux.enable(pipes[0].r);
}
