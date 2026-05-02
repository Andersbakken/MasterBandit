#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Lightweight always-on instrumentation counters. Read via `mb --ctl stats`
// and `mb --ctl wait-idle`. Updated on the event-loop thread today, but the
// atomics let readers from the IPC transport observe consistent values
// regardless of future threading changes.
namespace obs {

inline std::atomic<uint64_t> bytes_parsed{0};
inline std::atomic<uint64_t> frames_presented{0};

// steady_clock microseconds at the end of the most recent non-empty
// injectData call. 0 if no parse has occurred yet.
inline std::atomic<uint64_t> last_parse_time_us{0};

// Snapshot of frames_presented taken at the most recent parse. wait-idle
// considers the terminal idle when frames_presented has advanced past this.
inline std::atomic<uint64_t> frames_at_last_parse{0};

// Phase 1 + sync-output diagnostics. Counts incremented per occurrence,
// no allocation, no lock. Read via `mb --ctl stats`.
inline std::atomic<uint64_t> injects{0};                 // injectData() calls
inline std::atomic<uint64_t> snapshot_publishes{0};      // buildAndPublishSnapshotLocked() runs
inline std::atomic<uint64_t> snapshot_skipped_hold{0};   // publishSnapshotIfDue() skipped (mHold=true)
inline std::atomic<uint64_t> update_events{0};           // Update events fired from injectData
inline std::atomic<uint64_t> publish_and_fire_events{0}; // publishAndFireEvent calls (resize/scroll/etc.)

inline uint64_t now_us() noexcept
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Called after bytes have been handed to the VT parser. `len` is the number
// of bytes passed to injectData (post-filter, if filtering is in play).
inline void notifyParse(size_t len) noexcept
{
    if (len == 0) return;
    bytes_parsed.fetch_add(len, std::memory_order_relaxed);
    frames_at_last_parse.store(frames_presented.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    last_parse_time_us.store(now_us(), std::memory_order_release);
}

// Called after a frame has finished rendering (whether or not Present ran).
inline void notifyFrame() noexcept
{
    frames_presented.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace obs
