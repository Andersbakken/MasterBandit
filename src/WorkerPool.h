#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

class WorkerPool {
public:
    explicit WorkerPool(uint32_t numThreads = 0)
        : workReady_(0), allDone_(0)
    {
        if (numThreads == 0)
            numThreads = std::max(1u, std::thread::hardware_concurrency());
        for (uint32_t i = 0; i < numThreads; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }

    ~WorkerPool()
    {
        stopping_ = true;
        for (size_t i = 0; i < threads_.size(); ++i)
            workReady_.release();
        for (auto& t : threads_)
            t.join();
    }

    // Dispatch items through fn, block until all complete.
    void dispatch(const std::vector<uint32_t>& items, const std::function<void(uint32_t)>& fn)
    {
        if (items.empty()) return;

        work_ = &fn;
        remaining_.store(static_cast<int>(items.size()), std::memory_order_relaxed);

        {
            std::lock_guard lock(mutex_);
            for (uint32_t item : items)
                queue_.push_back(item);
        }

        for (size_t i = 0; i < items.size(); ++i)
            workReady_.release();

        allDone_.acquire();
    }

    uint32_t threadCount() const { return static_cast<uint32_t>(threads_.size()); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

private:
    void workerLoop()
    {
        while (true) {
            workReady_.acquire();
            if (stopping_) return;

            uint32_t item;
            {
                std::lock_guard lock(mutex_);
                item = queue_.front();
                queue_.pop_front();
            }

            (*work_)(item);

            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                allDone_.release();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::deque<uint32_t> queue_;
    const std::function<void(uint32_t)>* work_ = nullptr;
    std::atomic<int> remaining_{0};
    std::counting_semaphore<128> workReady_;
    std::binary_semaphore allDone_;
    std::atomic<bool> stopping_{false};
};
