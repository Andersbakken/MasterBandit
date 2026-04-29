#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Worker pool with two submission modes:
//
//   1. submit(fn) - fire-and-forget async task. Returns immediately.
//   2. dispatch(items, fn) - synchronous parallel batch. Blocks the
//      caller until every item in this specific batch has finished;
//      independent submit()s and other concurrent dispatch()s on the
//      same pool run in parallel and do not block this call.
//
// Both modes share one queue and one set of worker threads. dispatch()
// uses a per-batch counter + condvar so multiple dispatch() calls
// (and any number of submit()s) can be in flight simultaneously
// without one blocking the others.
class WorkerPool {
public:
    explicit WorkerPool(uint32_t numThreads = 0)
    {
        if (numThreads == 0) {
            unsigned hw = std::thread::hardware_concurrency();
            numThreads = hw == 0 ? 2u : std::min(hw, 8u);
        }
        for (uint32_t i = 0; i < numThreads; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }

    ~WorkerPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_)
            t.join();
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Fire-and-forget. Returns immediately; fn runs on some worker.
    void submit(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.emplace_back(std::move(fn));
        }
        cv_.notify_one();
    }

    // Run fn(item) for each item in parallel. Blocks until every item
    // in *this batch* has finished. Other batches and submit() tasks
    // continue concurrently.
    void dispatch(const std::vector<uint32_t>& items, const std::function<void(uint32_t)>& fn)
    {
        if (items.empty()) return;

        // Per-batch state, captured by the wrapper task below.
        struct Batch {
            std::mutex          m;
            std::condition_variable cv;
            std::atomic<int>    remaining;
            const std::function<void(uint32_t)>* fn;
        };
        Batch batch;
        batch.remaining.store(static_cast<int>(items.size()), std::memory_order_relaxed);
        batch.fn = &fn;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (uint32_t item : items) {
                queue_.emplace_back([&batch, item] {
                    (*batch.fn)(item);
                    if (batch.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard<std::mutex> lk(batch.m);
                        batch.cv.notify_one();
                    }
                });
            }
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lk(batch.m);
        batch.cv.wait(lk, [&]{
            return batch.remaining.load(std::memory_order_acquire) == 0;
        });
    }

    uint32_t threadCount() const { return static_cast<uint32_t>(threads_.size()); }

private:
    void workerLoop()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]{ return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    std::vector<std::thread>            threads_;
    std::mutex                          mutex_;
    std::condition_variable             cv_;
    std::deque<std::function<void()>>   queue_;
    bool                                stopping_ { false };
};
