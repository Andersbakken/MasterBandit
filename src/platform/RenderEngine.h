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
class PlatformDawn;
class TextSystem;
class Window;

class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

    void setPlatform(PlatformDawn* p) { platform_ = p; }

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
    const PaneRenderPrivate* paneRenderPrivate(Uuid paneId) const {
        auto it = paneRenderPrivate_.find(paneId);
        return it == paneRenderPrivate_.end() ? nullptr : &it->second;
    }

    // Destroy all render-thread-owned resources. Must be called after the
    // render thread has been joined and before nativeInstance_/device_ are
    // released. Safe to call multiple times.
    void shutdown();

    static std::string popupStateKey(Uuid paneId, const std::string& popupId) {
        return paneId.toString() + "/" + popupId;
    }

    // Different separator from popup so key spaces can't collide.
    static std::string embeddedStateKey(Uuid paneId, uint64_t lineId) {
        return paneId.toString() + ":" + std::to_string(lineId);
    }

private:
    void resolveRow(PaneRenderPrivate& rs, int row, FontData* font, float scale,
                    float pixelOriginX, float pixelOriginY);
    void renderTabBar();

    PlatformDawn* platform_ = nullptr;

    // Dawn core
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;
    std::unique_ptr<dawn::native::Instance> nativeInstance_;
    TexturePool texturePool_;
    Renderer renderer_;
    wgpu::Texture headlessComposite_;

    // Render-thread-only state
    std::unordered_map<Uuid, PaneRenderPrivate, UuidHash> paneRenderPrivate_;
    std::unordered_map<std::string, PaneRenderPrivate> popupRenderPrivate_;
    // Embedded terminals — headless children anchored to a Document line id.
    // Keyed as "<paneUuid>:<lineId>" (see embeddedStateKey).
    std::unordered_map<std::string, PaneRenderPrivate> embeddedRenderPrivate_;
    RenderFrameState frameState_;
    Uuid lastFocusedPaneId_;

    // Tab bar render resources
    PooledTexture* tabBarTexture_ = nullptr;
    std::vector<PooledTexture*> pendingTabBarRelease_;
    std::vector<ComputeState*> pendingComputeRelease_;

    // Textures extracted from destroyed render-private entries in the
    // current frame. The entries are erased before end-of-frame, so their
    // pendingRelease lists wouldn't be reachable by the map-walk drain;
    // we stash the handles here instead and merge at the drain site.
    std::vector<PooledTexture*> pendingDestroyRelease_;

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
    std::unordered_map<Uuid, PaneRenderPrivate, UuidHash>& paneRenderPrivateMap() { return paneRenderPrivate_; }
    std::unordered_map<std::string, PaneRenderPrivate>& popupRenderPrivateMap() { return popupRenderPrivate_; }
    std::unordered_map<std::string, PaneRenderPrivate>& embeddedRenderPrivateMap() { return embeddedRenderPrivate_; }
};
