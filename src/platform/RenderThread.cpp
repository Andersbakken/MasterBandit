#include "RenderThread.h"

#include <eventloop/EventLoop.h>

RenderThread::RenderThread() = default;

RenderThread::~RenderThread()
{
    stop();
}

void RenderThread::start()
{
    if (thread_.joinable()) return;
    renderStop_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { threadMain(); });
}

void RenderThread::stop()
{
    if (!thread_.joinable()) return;
    renderStop_.store(true, std::memory_order_release);
    wake();
    thread_.join();
}

void RenderThread::wake()
{
    {
        std::lock_guard<std::mutex> lk(renderCvMutex_);
        renderWake_.store(true, std::memory_order_release);
    }
    renderCv_.notify_one();
}

void RenderThread::postToMain(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(deferredMainMutex_);
        deferredMain_.push_back(std::move(fn));
    }
    EventLoop* el = host_.eventLoop ? host_.eventLoop() : nullptr;
    if (el) el->wakeup();
}

void RenderThread::drainDeferredMain()
{
    // Called on the main thread from onTick (without mutex_).
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lk(deferredMainMutex_);
        pending.swap(deferredMain_);
    }
    for (auto& fn : pending) fn();
}

void RenderThread::enqueueTerminalExit(Terminal* t)
{
    {
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        pendingExits_.push_back(t);
    }
    EventLoop* el = host_.eventLoop ? host_.eventLoop() : nullptr;
    if (el) el->wakeup();
}

void RenderThread::drainPendingExits()
{
    std::vector<Terminal*> exits;
    {
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        exits.swap(pendingExits_);
    }
    if (!host_.onTerminalExit) return;
    for (auto* t : exits) host_.onTerminalExit(t);
}

void RenderThread::applyPendingMutations()
{
    // Called on the main thread at end of tick. Acquires mutex_,
    // transfers pending_ into renderState_, clears pending_.
    std::lock_guard<std::recursive_mutex> plk(mutex_);

    // Drain terminal exits under the lock so that terminal destruction
    // can't race the render thread's use of frameState_ term pointers.
    drainPendingExits();

    // Transfer dirty flags. Note: tabBarDirty_ / dividersDirty_ main-thread
    // flags owned by PlatformDawn are merged and cleared inside
    // host_.buildRenderFrameState().
    renderState_.tabBarDirty     |= pending_.tabBarDirty;
    renderState_.dividersDirty   |= pending_.dividersDirty;
    renderState_.focusChanged    |= pending_.focusChanged;
    renderState_.surfaceNeedsReconfigure |= pending_.surfaceNeedsReconfigure;

    // Font atlas change flags
    renderState_.mainFontAtlasChanged   |= pending_.mainFontAtlasChanged;
    renderState_.tabBarFontAtlasChanged |= pending_.tabBarFontAtlasChanged;
    renderState_.mainFontRemoved        |= pending_.mainFontRemoved;
    renderState_.tabBarFontRemoved      |= pending_.tabBarFontRemoved;
    renderState_.viewportSizeChanged    |= pending_.viewportSizeChanged;

    // Rebuild the full shadow copy from live state.
    if (host_.buildRenderFrameState) host_.buildRenderFrameState();

    // Transfer per-pane dirty pane entries so the render thread picks
    // them up at snapshot time.
    for (int id : pending_.dirtyPanes) {
        renderState_.dividerGeoms[id]; // ensure entry exists (noop if already there)
    }

    // Divider geometry updates — when the layout changes (dividersDirty),
    // replace dividerGeoms entirely so stale entries from removed panes
    // don't linger and cause ghost dividers to be drawn.
    if (!pending_.dividerUpdates.empty() || pending_.dividersDirty ||
        !pending_.clearDividerPanes.empty()) {
        renderState_.dividerGeoms.clear();
    }
    for (auto& du : pending_.dividerUpdates) {
        DividerGeom& dg = renderState_.dividerGeoms[du.paneId];
        dg.x = du.x; dg.y = du.y; dg.w = du.w; dg.h = du.h;
        dg.r = du.r; dg.g = du.g; dg.b = du.b; dg.a = du.a;
        dg.valid = du.valid;
    }

    pending_.clear();
}

void RenderThread::threadMain()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lk(renderCvMutex_);
            renderCv_.wait(lk, [this] {
                return renderWake_.load(std::memory_order_acquire) ||
                       renderStop_.load(std::memory_order_acquire);
            });
            renderWake_.store(false, std::memory_order_relaxed);
        }
        if (renderStop_.load(std::memory_order_acquire)) return;
        if (host_.onFrame) host_.onFrame();
    }
}
