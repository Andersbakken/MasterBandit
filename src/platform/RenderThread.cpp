#include "RenderThread.h"

#include "Observability.h"
#include "PlatformDawn.h"
#include "Utils.h"  // overloaded<> helper for std::visit

#include <eventloop/EventLoop.h>
#include <spdlog/spdlog.h>

#include <variant>

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

void RenderThread::enqueueTerminalExit(Terminal* t)
{
    {
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        pendingExits_.push_back(t);
    }
    if (EventLoop* el = platform_->eventLoop_.get()) el->wakeup();
}

void RenderThread::drainPendingExits()
{
    std::vector<Terminal*> exits;
    {
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        exits.swap(pendingExits_);
    }
    for (auto* t : exits) platform_->terminalExited(t);
}

void RenderThread::applyPendingMutations()
{
    // [TIMING] Total duration of applyPendingMutations.
    const uint64_t mt0 = obs::now_us();
    // [TIMING] Wait time to acquire the render-thread mutex (queued
    // behind onKey, snapshotUnderLock, or applyPendingMutations from
    // any other path).
    const uint64_t lt0 = obs::now_us();
    // Called on the main thread at end of tick. Acquires mutex_,
    // transfers pending_ into renderState_, clears pending_.
    std::lock_guard<std::recursive_mutex> plk(mutex_);
    if (auto dt = obs::now_us() - lt0; dt > 1000)
        spdlog::warn("[TIMING] applyPendingMutations: lock wait {} us", dt);

    // Drain terminal exits under the lock so that terminal destruction
    // can't race the render thread's use of frameState_ term pointers.
    drainPendingExits();

    // Transfer dirty flags. Note: tabBarDirty_ / dividersDirty_ main-thread
    // flags owned by PlatformDawn are merged and cleared inside
    // buildRenderFrameState().
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

    // Texture / row-cache release requests. Accumulate: if multiple ticks
    // elapse between render frames, every request still reaches the render
    // thread on the next snapshot.
    renderState_.releasePaneTextureIds.insert(
        renderState_.releasePaneTextureIds.end(),
        pending_.releasePaneTextures.begin(),
        pending_.releasePaneTextures.end());
    renderState_.releasePopupTextureKeys.insert(
        renderState_.releasePopupTextureKeys.end(),
        pending_.releasePopupTextures.begin(),
        pending_.releasePopupTextures.end());
    renderState_.releaseEmbeddedTextureKeys.insert(
        renderState_.releaseEmbeddedTextureKeys.end(),
        pending_.releaseEmbeddedTextures.begin(),
        pending_.releaseEmbeddedTextures.end());
    renderState_.releaseAllPaneTextures |= pending_.releaseAllPaneTextures;
    renderState_.releaseTabBarTexture   |= pending_.releaseTabBarTexture;
    renderState_.invalidateAllRowCaches |= pending_.invalidateAllRowCaches;

    // Distill structural ops down to destroys. Create* and Resize* are
    // redundant with the auto-heal path in renderFrame (first-use sizing),
    // so we only need to propagate the removals.
    for (const auto& op : pending_.structuralOps) {
        std::visit(overloaded {
            [&](const PendingMutations::CreatePaneState&)  {},
            [&](const PendingMutations::ResizePaneState&)  {},
            [&](const PendingMutations::CreatePopupState&) {},
            [&](const PendingMutations::ResizePopupState&) {},
            [&](const PendingMutations::DestroyPaneState& d) {
                renderState_.destroyedPaneIds.push_back(d.paneId);
            },
            [&](const PendingMutations::DestroyPopupState& d) {
                renderState_.destroyedPopupKeys.push_back(
                    d.paneId.toString() + "/" + d.popupId);
            },
            [&](const PendingMutations::DestroyEmbeddedState& d) {
                renderState_.destroyedEmbeddedKeys.push_back(
                    d.paneId.toString() + ":" + std::to_string(d.lineId));
            },
        }, op);
    }

    // Rebuild the full shadow copy from live state.
    const uint64_t bt0 = obs::now_us();
    platform_->buildRenderFrameState();
    if (auto dt = obs::now_us() - bt0; dt > 1000)
        spdlog::warn("[TIMING] buildRenderFrameState: {} us", dt);

    // Transfer per-pane dirty pane entries so the render thread picks
    // them up at snapshot time.
    for (const Uuid& id : pending_.dirtyPanes) {
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

    if (auto dt = obs::now_us() - mt0; dt > 1000)
        spdlog::warn("[TIMING] applyPendingMutations: total {} us", dt);
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
        // Tick Dawn on the render thread: drains device-side events
        // (completion callbacks, deferred destroys). Safe to run without
        // the render-thread mutex because all GPU calls happen on this
        // thread only.
        RenderEngine* re = platform_->renderEngine_.get();
        if (!re) continue;
        re->device().Tick();
        // renderFrame manages the render-thread mutex internally via the
        // Host snapshot callback — it releases the lock during shaping
        // so input handlers / deferred callbacks on the main thread can
        // proceed concurrently with the CPU-heavy section.
        re->renderFrame();
    }
}
