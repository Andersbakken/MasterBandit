#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

// Latched value with a timed wait.
//
// State is either Ready(T) or Pending. Producer threads call set(v) to
// publish a value; consumers call getOrWait(timeout) which returns
// immediately if Ready, otherwise sleeps on a condvar up to `timeout`.
//
// set() is multi-shot: producers may call it repeatedly to update the
// stored value. Consumers always observe the latest value at the moment
// they read. unset() reverts to Pending so the next consumer call has to
// wait again — useful when a change-notification source signals "value is
// stale" without carrying the new value inline.
//
// notify_all() on every set() so multiple waiters are correct, though the
// expected usage is a single waiter.
template <typename T>
class WaitableValue {
public:
    WaitableValue() = default;
    WaitableValue(const WaitableValue&) = delete;
    WaitableValue& operator=(const WaitableValue&) = delete;

    void set(T v)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            value_ = std::move(v);
        }
        cv_.notify_all();
    }

    void unset()
    {
        std::lock_guard<std::mutex> lk(mu_);
        value_.reset();
    }

    bool ready() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return value_.has_value();
    }

    // Read without waiting. nullopt if Pending.
    std::optional<T> tryGet() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return value_;
    }

    // Read with a timed wait. nullopt on timeout.
    std::optional<T> getOrWait(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, timeout, [&] { return value_.has_value(); });
        return value_;
    }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::optional<T>        value_;
};
