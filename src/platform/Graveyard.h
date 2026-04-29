#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Deferred-destruction queue for objects whose lifetime must extend past
// the render thread's current use of RenderEngine::frameState_. The render
// thread may hold raw pointers into a Terminal (popup / pane / overlay)
// after it has released the platform mutex and until its renderFrame()
// returns. Destroying those objects synchronously on the main thread would
// race with the render thread's lockless dereference.
//
// Contract: defer() is called on the main thread under the render-thread
// mutex, stamped with RenderThread::completedFrames() read at the same
// moment. sweep() runs on the main thread and frees entries whose stamp is
// strictly less than the current completedFrames() — i.e. at least one
// full renderFrame has completed since the entry was staged, so any frame
// that could have been mid-flight at stamp time has ended and its local
// references into frameState_ are out of scope.
class Graveyard {
public:
    Graveyard() = default;
    ~Graveyard() = default;

    Graveyard(const Graveyard&) = delete;
    Graveyard& operator=(const Graveyard&) = delete;

    // Take ownership of a unique_ptr; destroy after one render frame.
    // `ready` (optional) is consulted in addition to the frame-stamp
    // gate: even if completedFrames has surpassed `stamp`, the entry is
    // held until `ready()` returns true. Used to keep a Terminal alive
    // until any in-flight parse worker that captured `this` has
    // returned.
    template <typename T>
    void defer(std::unique_ptr<T> ptr, uint64_t stamp,
               std::function<bool()> ready = {}) {
        if (!ptr) return;
        std::shared_ptr<void> holder(ptr.release(), [](void* p) {
            delete static_cast<T*>(p);
        });
        entries_.push_back({stamp, std::move(holder), std::move(ready)});
    }

    // Take ownership of a value (moved into a heap allocation).
    template <typename T>
    void deferValue(T&& val, uint64_t stamp) {
        using U = std::decay_t<T>;
        auto ptr = std::make_unique<U>(std::forward<T>(val));
        defer(std::move(ptr), stamp);
    }

    // Called on the main thread each tick. Frees entries whose stamp has
    // been surpassed by at least one completed renderFrame AND whose
    // optional `ready` predicate returns true.
    void sweep(uint64_t completedFrames) {
        auto it = std::remove_if(entries_.begin(), entries_.end(),
            [completedFrames](const Entry& e) {
                if (completedFrames <= e.stamp) return false;
                if (e.ready && !e.ready()) return false;
                return true;
            });
        entries_.erase(it, entries_.end());
    }

    // Force-drain all entries (used at shutdown once the render thread
    // has joined).
    void drainAll() { entries_.clear(); }

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t stamp;
        std::shared_ptr<void> holder;
        std::function<bool()> ready; // empty == always ready (only stamp gates)
    };
    std::vector<Entry> entries_;
};
