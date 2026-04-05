#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

class WorkerPool {
public:
    explicit WorkerPool(unsigned int numThreads = 0)
        : workReady_(0), allDone_(0)
    {
        if (numThreads == 0)
            numThreads = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned int i = 0; i < numThreads; ++i)
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
    void dispatch(const std::vector<int>& items, const std::function<void(int)>& fn)
    {
        if (items.empty()) return;

        work_ = &fn;
        remaining_.store(static_cast<int>(items.size()), std::memory_order_relaxed);

        {
            std::lock_guard lock(mutex_);
            for (int item : items)
                queue_.push_back(item);
        }

        for (size_t i = 0; i < items.size(); ++i)
            workReady_.release();

        allDone_.acquire();
    }

    unsigned int threadCount() const { return static_cast<unsigned int>(threads_.size()); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

private:
    void workerLoop()
    {
        while (true) {
            workReady_.acquire();
            if (stopping_) return;

            int item;
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
    std::deque<int> queue_;
    const std::function<void(int)>* work_ = nullptr;
    std::atomic<int> remaining_{0};
    std::counting_semaphore<128> workReady_;
    std::binary_semaphore allDone_;
    std::atomic<bool> stopping_{false};
};
