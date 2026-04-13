#pragma once

#include <atomic>
#include <sys/types.h>
#include "InputTypes.h"
#include "ClickDetector.h"
#include "Terminal.h"
#include "TerminalSnapshot.h"
#include "Renderer.h"
#include "TexturePool.h"
#include "text.h"
#include "DebugIPC.h"
#include "FontFallback.h"
#include "Layout.h"
#include "Pane.h"
#include "Tab.h"
#include "Action.h"
#include "Bindings.h"
#include "WorkerPool.h"
#include "ScriptEngine.h"

#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#include <eventloop/EventLoop.h>
#include <eventloop/Window.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Custom spdlog sink that forwards to DebugIPC
class DebugIPCSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void setIPC(DebugIPC* ipc) { ipc_ = ipc; }
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (!ipc_) return;
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        ipc_->broadcastLog(std::string(formatted.data(), formatted.size()));
    }
    void flush_() override {}
private:
    DebugIPC* ipc_ = nullptr;
};

// Implemented in platform-specific files (PlatformUtils_macOS.mm / PlatformUtils_Linux.cpp)
bool platformIsDarkMode();
void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback);
void platformSendNotification(const std::string& title, const std::string& body);
void platformOpenURL(const std::string& url);
std::string platformProcessCWD(pid_t pid);

class PlatformDawn {
public:
    enum Flag : uint32_t {
        FlagNone     = 0,
        FlagHeadless = 1 << 0,
        FlagIPC      = 1 << 1,
    };

    PlatformDawn(int argc, char** argv, uint32_t flags = FlagNone);
    ~PlatformDawn();

    int exec();
    void quit(int status = 0);
    void createTerminal(const TerminalOptions& options);
    void createTab();
    void closeTab(int idx);

    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }
    TexturePool& texturePool() { return texturePool_; }
    bool isHeadless() const { return flags_ & FlagHeadless; }
    bool hasFlag(Flag f) const { return flags_ & f; }

    void setTestConfig(const std::string& fontPath, int cols, int rows, float fontSize,
                       const std::string& emojiFontPath = {},
                       const std::string& fallbackFontPath = {}) {
        testFontPath_ = fontPath;
        testCols_ = cols;
        testRows_ = rows;
        testFontSize_ = fontSize;
        testEmojiFontPath_ = emojiFontPath;
        testFallbackFontPath_ = fallbackFontPath;
    }

    const std::string& exeDir() const { return exeDir_; }

    // Input handlers (called from Window callbacks)
    void onKey(int key, int scancode, int action, int mods);
    void onChar(uint32_t codepoint);
    void onFramebufferResize(int width, int height);
    void adjustFontSize(float delta);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    void renderFrame();
    void setNeedsRedraw();

    // Map a CSS / kitty pointer name (from OSC 22) to a Window::CursorStyle.
    // Empty string and unknown names fall back to the platform default arrow.
    static Window::CursorStyle pointerShapeNameToCursorStyle(const std::string& name);

    // Per-pane cached cursor style requested via OSC 22. Looked up on every
    // mouse move (cheap unordered_map find). Absent entry / Arrow → fall back
    // to the IBeam used for selection. Updated by the onMouseCursorShape
    // terminal callback; erased on pane destruction.
    std::unordered_map<int, Window::CursorStyle> paneCursorStyle_;
    // Push the active tab's focused-pane cursor style to the window. Call
    // after focus changes (pane- or tab-level) so the cursor updates without
    // waiting for the next mouse-move event.
    void refreshPointerShape();

    bool shouldClose() {
        if (isHeadless()) return false;
        return window_ && window_->shouldClose();
    }

    std::string gridToJson(int id);
    std::string statsJson(int id);

private:
    void configureSurface(uint32_t width, uint32_t height);

    void addPtyPoll(int fd, Terminal* term);
    void removePtyPoll(int fd);

    // Dawn core
    wgpu::Device device_;
    wgpu::Queue queue_;
    std::unique_ptr<dawn::native::Instance> nativeInstance_;
    TexturePool texturePool_;
    int exitStatus_ = 0;
    bool running_ = false;

    uint32_t flags_ = FlagNone;
    bool platformInitialized_ = false;
    int testCols_ = 80;
    int testRows_ = 24;
    float testFontSize_ = 16.0f;
    std::string testFontPath_;
    std::string testEmojiFontPath_;
    std::string testFallbackFontPath_;
    wgpu::Texture headlessComposite_;  // offscreen composite target (headless, with CopyDst)
    std::unique_ptr<EventLoop> eventLoop_;
    std::unique_ptr<Window>    window_;
    EventLoop::TimerId         configDebounceTimer_ = 0;
    bool                       configDebounceActive_ = false;

    // Cursor blink: a single repeating timer toggles a phase flag and requests
    // a redraw. The renderer hides cursors that are currently blinking when the
    // phase is off. interval=0 disables blinking.
    EventLoop::TimerId         cursorBlinkTimer_ = 0;
    int                        cursorBlinkInterval_ = 500;
    // Written by the blink timer on the main thread, read by the render
    // thread in renderFrame.
    std::atomic<bool>          cursorBlinkPhaseOn_ { true };
    void                       applyBlinkInterval(int ms);

    // Animation wakeup: one-shot timer scheduled for the next animated-image
    // frame boundary. Avoids spinning the event loop at display refresh rate
    // while waiting for the next tick.
    EventLoop::TimerId         animationTimer_ = 0;
    uint64_t                   animationTimerDueAt_ = 0;

    // Resize coalescing (Phase 6). During a live window drag, Cocoa fires
    // framebuffer-resize events at display cadence. Each triggers surface
    // reconfigure + layout recompute + terminal reflow — expensive to run
    // per frame. Debounce to 25 ms so a drag settles briefly before we do
    // the work; on live-resize-end, flush immediately.
    EventLoop::TimerId         resizeDebounceTimer_ = 0;
    uint32_t                   pendingResizeW_ = 0;
    uint32_t                   pendingResizeH_ = 0;
    void                       applyFramebufferResize(int width, int height);
    void                       flushPendingFramebufferResize();
    // Render thread requests an animation wakeup by setting this atomic.
    // Main thread consumes it in onTick and schedules the actual timer on
    // the event loop (thread-affine).
    std::atomic<uint64_t>      pendingAnimationDueAt_ { 0 };
    void                       scheduleAnimationWakeup(uint64_t dueAt);
    void                       applyPendingAnimationWakeup();
    std::string exeDir_;
    std::unique_ptr<DebugIPC> debugIPC_;
    std::shared_ptr<DebugIPCSink> debugSink_;

    // Tabs
    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeTabIdx_ = 0;
    int lastFocusedPaneId_ = -1;

    void updateTabBarVisibility() {
        if (tabBarConfig_.style != "auto") return;
        bool visible = tabBarVisible();
        int h = visible ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
        for (auto& tab : tabs_) {
            tab->layout()->setTabBar(h, tabBarConfig_.position);
            tab->layout()->computeRects(fbWidth_, fbHeight_);
            for (auto& p : tab->layout()->panes())
                p->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);
        }
        if (visible) {
            // Refresh tab titles from foreground process for tabs that
            // have no OSC title — the callback may have fired before the
            // tab bar existed or before the tab was added to tabs_.
            for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
                Tab* tab = tabs_[i].get();
                Pane* fp = tab->layout()->focusedPane();
                if (fp && fp->title().empty() && tab->title().empty()) {
                    if (auto* t = fp->terminal()) {
                        std::string proc = t->foregroundProcess();
                        if (!proc.empty())
                            tab->setTitle(proc);
                    }
                }
            }
        }
    }

    bool tabBarVisible() const {
        if (tabBarConfig_.style == "hidden") return false;
        if (tabBarConfig_.style == "auto") return tabs_.size() > 1;
        return true;
    }

    Tab* activeTab() {
        if (tabs_.empty() || activeTabIdx_ < 0 || activeTabIdx_ >= static_cast<int>(tabs_.size()))
            return nullptr;
        return tabs_[activeTabIdx_].get();
    }

    void notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn) {
        for (auto& tab : tabs_) {
            for (auto& [paneId, _] : paneRenderStates_) {
                Pane* pane = tab->layout()->pane(paneId);
                if (pane) {
                    if (auto* term = pane->terminal()) fn(term);
                }
            }
        }
    }

    Terminal* activeTerm() {
        Tab* tab = activeTab();
        if (!tab) return nullptr;
        if (tab->hasOverlay()) return tab->topOverlay();
        Pane* pane = tab->layout()->focusedPane();
        return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
    }

    // Shared rendering state
    wgpu::Surface surface_;
    Renderer renderer_;
    TextSystem textSystem_;
    std::string fontName_ = "mono";
    float fontSize_ = 16.0f;
    float baseFontSize_ = 0.0f;
    std::string replacementChar_ = "\xEF\xBF\xBD";
    float charWidth_ = 0.0f;
    float lineHeight_ = 0.0f;
    float contentScaleX_ = 1.0f, contentScaleY_ = 1.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;
    std::string primaryFontPath_;
    FontFallback fontFallback_;

    // Default colors
    uint32_t defaultFgColor_ = 0xFFDDDDDD;
    uint32_t defaultBgColor_ = 0x00000000;
    std::atomic<bool> needsRedraw_ { true };
    std::atomic<int> pendingGpuCallbacks_ { 0 };
    bool controlPressed_ = false;
    uint32_t lastMods_ = 0;

    // Tracked by onCursorPos / onMouseButton (replaces glfwGetCursorPos / glfwGetMouseButton)
    double lastCursorX_ = 0.0, lastCursorY_ = 0.0;
    uint32_t heldButtons_ = 0;  // bitmask of Button values currently pressed

    // Per-pane render state
    struct PaneRenderState {
        // Phase 1 render-thread decoupling: renderer reads terminal state from
        // this snapshot, not the live Terminal. Populated once per frame via
        // snapshot.update(*term). See RENDER_THREADING.md.
        TerminalSnapshot snapshot;
        std::vector<ResolvedCell> resolvedCells;
        std::vector<GlyphEntry> glyphBuffer;
        uint32_t totalGlyphs = 0;
        int lastCursorX = -1, lastCursorY = -1;
        bool lastCursorVisible = true;
        bool lastCursorBlinkOn = true;  // last rendered blink phase, for blinking cursors
        // Image IDs visible in this pane as of its last re-render. Used to keep
        // the GPU image cache resident for panes that aren't re-rendering this
        // frame (heldTexture is displaying them; eviction would just force a
        // re-upload on the next real render).
        std::unordered_set<uint32_t> lastVisibleImageIds;
        PooledTexture* heldTexture = nullptr;
        bool dirty = true;
        std::vector<PooledTexture*> pendingRelease;
        wgpu::Buffer dividerVB;
        // Cached popup border buffers: 4 buffers (top/bottom/left/right) per popup
        struct PopupBorder {
            std::string popupId;
            int cellX = 0, cellY = 0, cellW = 0, cellH = 0;
            wgpu::Buffer top, bottom, left, right;
        };
        std::vector<PopupBorder> popupBorders;
        int lastViewportOffset = 0;
        int lastHistorySize = 0;
        // Previous frame's selection — used to force re-resolve of
        // newly-selected and newly-deselected rows in the snapshot world
        // (selection mutations no longer mark grid rows dirty).
        TerminalEmulator::Selection lastSelection{};

        struct RowGlyphCache {
            std::vector<GlyphEntry> glyphs;
            std::vector<std::pair<uint32_t, uint32_t>> cellGlyphRanges;
            std::vector<Renderer::ColrDrawCmd> colrDrawCmds;  // per-row COLR quads
            std::vector<Renderer::ColrRasterCmd> colrRasterCmds;  // per-row new COLR tiles
            bool valid = false;
        };
        std::vector<RowGlyphCache> rowShapingCache;
    };
    std::unordered_map<int, PaneRenderState> paneRenderStates_;
    std::unordered_map<Tab*, PaneRenderState> overlayRenderStates_; // per-tab overlay render state
    std::unordered_map<std::string, PaneRenderState> popupRenderStates_; // keyed by "<paneId>/<popupId>"

    static std::string popupStateKey(int paneId, const std::string& popupId) {
        return std::to_string(paneId) + "/" + popupId;
    }
    void releasePopupStates(Pane* pane);

    void resolveRow(PaneRenderState& rs, int row, FontData* font, float scale,
                    float pixelOriginX, float pixelOriginY);

    std::unordered_map<int, Terminal*> ptyPolls_;

    TerminalOptions terminalOptions_;

    // Tab bar
    TabBarConfig  tabBarConfig_;
    std::string   tabBarFontName_   = "tab_bar";
    float         tabBarFontSize_   = 0.0f;
    float         tabBarCharWidth_  = 0.0f;
    float         tabBarLineHeight_ = 0.0f;
    int           tabBarCols_       = 0;
    PooledTexture* tabBarTexture_   = nullptr;
    bool          tabBarDirty_      = true;
    int           tabBarAnimFrame_  = 0;
    uint64_t      lastAnimTick_     = 0;
    std::vector<PooledTexture*> pendingTabBarRelease_;
    std::vector<ComputeState*> pendingComputeRelease_;

    // Deferred release from GPU completion callbacks (thread-safe)
    std::mutex deferredReleaseMutex_;
    std::vector<PooledTexture*> deferredTextureRelease_;
    std::vector<ComputeState*> deferredComputeRelease_;

    // Render thread. renderFrame() runs here, woken by main thread after
    // parse / input / animation mutations. Coarse `platformMutex_` serializes
    // render reads against main-thread structural mutations (tab/pane/popup
    // create/destroy, tab switch, resize). Terminal state is separately
    // protected by TerminalEmulator::mutex() — acquired briefly inside
    // TerminalSnapshot::update().
    std::mutex              platformMutex_;
    std::mutex              renderCvMutex_;
    std::condition_variable renderCv_;
    std::atomic<bool>       renderWake_ { false };
    std::atomic<bool>       renderStop_ { false };
    std::thread             renderThread_;
    void renderThreadMain();
    void wakeRenderThread();

    // Deferred structural mutations from parse callbacks. Parse runs on the
    // main thread without `platformMutex_` held so the render thread can
    // draw concurrently during a flood. Callbacks that mutate PlatformDawn
    // structural state (notably `terminalExited`, which tears down a
    // pane/tab) push themselves here; the queue drains after parse, under
    // `platformMutex_`, avoiding the Terminal-mutex / platformMutex_ lock
    // inversion a synchronous call would create.
    std::mutex                            deferredExitMutex_;
    std::vector<Terminal*>                pendingExits_;
    void drainPendingExits();

    // Generic main-thread post queue. Parse-time callbacks (title / icon /
    // cwd / progress / mouse-cursor-shape / foreground-process changes)
    // enqueue a lambda via postToMainThread(); drainDeferredMain() runs
    // them on the main thread under platformMutex_ after parse completes.
    // (std::move_only_function would let us capture move-only types but
    // isn't in libc++ yet on AppleClang — std::function suffices.)
    std::mutex                            deferredMainMutex_;
    std::vector<std::function<void()>>    deferredMain_;
    void postToMainThread(std::function<void()> fn);
    void drainDeferredMain();

    // Pane divider colors
    float dividerR_ = 0.24f, dividerG_ = 0.24f, dividerB_ = 0.24f, dividerA_ = 1.0f;
    int   dividerWidth_ = 1;

    // Pane tints
    float activeTint_[4]   = {1.0f, 1.0f, 1.0f, 1.0f};
    float inactiveTint_[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Tab bar colors
    uint32_t tbBgColor_        = 0xFF261926;
    uint32_t tbActiveBgColor_  = 0xFFf7a27a;
    uint32_t tbActiveFgColor_  = 0xFF261a1b;
    uint32_t tbInactiveBgColor_= 0xFF3b2824;
    uint32_t tbInactiveFgColor_= 0xFF895f56;
    float progressColorR_ = 0.0f, progressColorG_ = 0.6f, progressColorB_ = 1.0f;
    float progressBarHeight_ = 3.0f;

    // Scaled padding in pixels
    float padLeft_ = 0, padTop_ = 0, padRight_ = 0, padBottom_ = 0;

    void initTabBar(const TabBarConfig& cfg);
    void renderTabBar();

    void reloadConfigNow();
    void applyFontChange(const Config& config);
    void invalidateAllRowCaches();

    // Keybindings
    std::vector<Binding> bindings_;
    SequenceMatcher      sequenceMatcher_;
    void dispatchAction(const Action::Any& action);
    void terminalExited(Terminal* terminal);

    // Mouse bindings
    std::vector<MouseBinding> mouseBindings_;
    ClickDetector clickDetector_;
    bool selectionDragActive_ = false;
    bool selectionDragStarted_ = false;
    double selectionDragOriginX_ = 0;
    double selectionDragOriginY_ = 0;
    EventLoop::TimerId autoScrollTimer_ = 0;
    bool autoScrollTimerActive_ = false;
    int  autoScrollDir_ = 0;  // +1 = scroll into history (up), -1 = toward live (down)
    int  autoScrollCol_ = 0;  // column to use when synthesizing boundary move events
    void startAutoScroll(int dir, int col);
    void stopAutoScroll();
    void doAutoScroll();
    struct MouseContext {
        int cellCol = 0, cellRow = 0;
        int pixelX = 0, pixelY = 0;
        int tabBarClickIndex = -1;
        MouseButton button = MouseButton::Left;
    } mouseCtx_;
    MouseRegion hitTest(double sx, double sy);

    // Action listeners
    Action::Dispatcher actionDispatcher_;

    // Worker pool for parallel row resolution
    WorkerPool renderWorkers_;

    // Script engine
    Script::Engine scriptEngine_;

    // Callback/terminal helpers
    TerminalCallbacks buildTerminalCallbacks(int paneId);

    // Pane helpers
    void spawnTerminalForPane(Pane* pane, int tabIdx, const std::string& cwd = {});
    void resizeAllPanesInTab(Tab* tab);
    void refreshDividers(Tab* tab);
    void clearDividers(Tab* tab);
    void releaseTabTextures(Tab* tab);
    void updateTabTitleFromFocusedPane(int tabIdx);
    void updateWindowTitle();
    void notifyPaneFocusChange(Tab* tab, int prevId, int newId);
};

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, uint32_t flags = PlatformDawn::FlagNone);
