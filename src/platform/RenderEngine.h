#pragma once

#include "Renderer.h"
#include "RenderSync.h"
#include "TexturePool.h"
#include "TerminalSnapshot.h"
#include "WorkerPool.h"

#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

class DebugIPC;
class TextSystem;
class Window;

class RenderEngine {
public:
    struct Host {
        TextSystem* textSystem = nullptr;

        // Snapshot: under platformMutex_, consume one-shot flags from
        // renderState_ after copying. Returns false if the frame should be skipped.
        std::function<bool(RenderFrameState& out)> snapshotUnderLock;

        // Under platformMutex_, if renderState_.surfaceNeedsReconfigure,
        // clear it and return {true, fbWidth_, fbHeight_}. Else {false, 0, 0}.
        std::function<std::tuple<bool, uint32_t, uint32_t>()> takeSurfaceReconfigureRequest;
        std::function<bool()> takeViewportSizeChangedRequest;

        // Current fb size (main thread updates, under lock).
        std::function<std::pair<uint32_t, uint32_t>()> currentFbSize;

        // Render thread posts a lambda for main-thread execution.
        std::function<void(std::function<void()>)> postToMain;

        // Render thread requests an animation wakeup.
        std::function<void(uint64_t dueAtNs)> scheduleAnimationAt;

        // DebugIPC* (may be null).
        std::function<DebugIPC*()> debugIPC;

        // Called on the render thread at the end of every renderFrame()
        // invocation (including early returns), after all local pointers
        // into frameState_ have gone out of scope. Bumps the
        // RenderThread::completedFrames() counter that the main-thread
        // Graveyard uses to know when deferred objects are safe to free.
        std::function<void()> notifyFrameCompleted;

        bool headless = false;
    };

    RenderEngine();
    ~RenderEngine();

    void setHost(Host host) { host_ = std::move(host); }

    // One-time GPU initialization. Selects the adapter, creates device/queue,
    // initializes the texture pool. Returns false on failure.
    bool initGpu();

    // Windowed mode only: create the surface from the given Window.
    bool createSurface(Window* window);

    // Shader + renderer initialization (must be called after initGpu).
    bool initRenderer(const std::string& shaderDir, uint32_t fbWidth, uint32_t fbHeight);

    // Called after initRenderer and initial font registration to upload atlas.
    void uploadFontAtlas(const std::string& fontName, const FontData& font);

    void configureSurface(uint32_t width, uint32_t height);

    // Top-level render thread entry point.
    void renderFrame();

    // Accessors.
    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }
    wgpu::Surface surface() const { return surface_; }
    dawn::native::Instance* nativeInstance() { return nativeInstance_.get(); }
    TexturePool& texturePool() { return texturePool_; }
    Renderer& renderer() { return renderer_; }

    std::atomic<bool>& needsRedrawFlag() { return needsRedraw_; }
    bool needsRedraw() const { return needsRedraw_.load(std::memory_order_acquire); }
    void setNeedsRedraw() { needsRedraw_ = true; }
    int pendingGpuCallbacks() const { return pendingGpuCallbacks_.load(std::memory_order_acquire); }

    // Per-pane render state queries used by Platform_Debug.cpp.
    const PaneRenderPrivate* paneRenderPrivate(int paneId) const {
        auto it = paneRenderPrivate_.find(paneId);
        return it == paneRenderPrivate_.end() ? nullptr : &it->second;
    }

    // Destroy all render-thread-owned resources. Must be called after the
    // render thread has been joined and before nativeInstance_/device_ are
    // released. Safe to call multiple times.
    void shutdown();

    static std::string popupStateKey(int paneId, const std::string& popupId) {
        return std::to_string(paneId) + "/" + popupId;
    }

private:
    void resolveRow(PaneRenderPrivate& rs, int row, FontData* font, float scale,
                    float pixelOriginX, float pixelOriginY);
    void renderTabBar();

    Host host_;

    // Dawn core
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;
    std::unique_ptr<dawn::native::Instance> nativeInstance_;
    TexturePool texturePool_;
    Renderer renderer_;
    wgpu::Texture headlessComposite_;

    // Render-thread-only state
    std::unordered_map<int, PaneRenderPrivate> paneRenderPrivate_;
    std::unordered_map<std::string, PaneRenderPrivate> popupRenderPrivate_;
    PaneRenderPrivate overlayRenderPrivate_;
    RenderFrameState frameState_;
    int lastFocusedPaneId_ = -1;

    // Tab bar render resources
    PooledTexture* tabBarTexture_ = nullptr;
    std::vector<PooledTexture*> pendingTabBarRelease_;
    std::vector<ComputeState*> pendingComputeRelease_;

    std::atomic<bool> needsRedraw_ { true };
    std::atomic<int> pendingGpuCallbacks_ { 0 };

    // Deferred release from GPU completion callbacks. The callbacks are
    // registered via queue_.OnSubmittedWorkDone with CallbackMode::AllowSpontaneous,
    // which means Dawn may fire them on its own completion thread even after
    // ~RenderEngine has run. Wrapping the mutex + vectors in a shared_ptr
    // lets the state outlive the outer object.
    struct DeferredReleaseState {
        std::mutex mutex;
        std::vector<PooledTexture*> textures;
        std::vector<ComputeState*> compute;
    };
    std::shared_ptr<DeferredReleaseState> deferredReleaseState_
        = std::make_shared<DeferredReleaseState>();

    // Worker pool for parallel row resolution
    WorkerPool renderWorkers_;

    bool shutdownCalled_ = false;

    friend class PlatformDawn;

public:
    // Access hooks for PlatformDawn during pane/popup lifecycle (main thread,
    // with platformMutex_ held so the render thread isn't touching these).
    std::unordered_map<int, PaneRenderPrivate>& paneRenderPrivateMap() { return paneRenderPrivate_; }
    std::unordered_map<std::string, PaneRenderPrivate>& popupRenderPrivateMap() { return popupRenderPrivate_; }
    PaneRenderPrivate& overlayRenderPrivate() { return overlayRenderPrivate_; }
};
