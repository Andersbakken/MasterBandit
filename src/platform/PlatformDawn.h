#pragma once

#include <atomic>
#include <sys/types.h>
#include "ActionRouter.h"
#include "AnimationScheduler.h"
#include "ConfigLoader.h"
#include "InputController.h"
#include "InputTypes.h"
#include "ClickDetector.h"
#include "TabManager.h"
#include "Terminal.h"
#include "TerminalSnapshot.h"
#include "Renderer.h"
#include "RenderEngine.h"
#include "RenderSync.h"
#include "RenderThread.h"
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

    wgpu::Device device() const { return renderEngine_ ? renderEngine_->device() : wgpu::Device{}; }
    wgpu::Queue queue() const { return renderEngine_ ? renderEngine_->queue() : wgpu::Queue{}; }
    TexturePool& texturePool() { return renderEngine_->texturePool(); }
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

    void onFramebufferResize(int width, int height);
    void adjustFontSize(float delta);

    void setNeedsRedraw();

    InputController* inputController() { return inputController_.get(); }

    bool shouldClose() {
        if (isHeadless()) return false;
        return window_ && window_->shouldClose();
    }

    std::string gridToJson(int id);
    std::string statsJson(int id);

private:
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
    std::unique_ptr<EventLoop> eventLoop_;
    std::unique_ptr<Window>    window_;

    // Blink / animation wakeup / resize debounce timers live in
    // AnimationScheduler. Opacity is snapshotted into RenderFrameState.
    std::unique_ptr<AnimationScheduler> animScheduler_;
    // Config hot-reload: owns the file watcher debounce timer and the
    // reloadNow() entry point. Calls back into applyConfig() on success.
    std::unique_ptr<ConfigLoader> configLoader_;
    // Owns Dawn device/queue/surface, Renderer, TexturePool, and all
    // render-thread-only state (pane/popup/overlay private state, frameState_,
    // tab-bar GPU texture). Created early in ctor so accessors work.
    std::unique_ptr<RenderEngine> renderEngine_;
    void                       onBlinkTick();
    void                       onResizeDebounceFire(uint32_t w, uint32_t h);
    void                       applyFramebufferResize(int width, int height);
    void                       flushPendingFramebufferResize();
    std::string exeDir_;
    std::unique_ptr<DebugIPC> debugIPC_;
    std::shared_ptr<DebugIPCSink> debugSink_;

    // Tabs — owned by TabManager.
    std::unique_ptr<TabManager> tabManager_;

    void updateTabBarVisibility() {
        if (tabBarConfig_.style != "auto") return;
        bool visible = tabBarVisible();
        int h = visible ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
        for (auto& tab : tabManager_->tabs()) {
            tab->layout()->setTabBar(h, tabBarConfig_.position);
            tab->layout()->computeRects(fbWidth_, fbHeight_);
            for (auto& p : tab->layout()->panes())
                p->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);
        }
        if (visible) {
            // Refresh tab titles from foreground process for tabs that
            // have no OSC title — the callback may have fired before the
            // tab bar existed or before the tab was added to tabs.
            auto& tabs = tabManager_->tabs();
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                Tab* tab = tabs[i].get();
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
        if (tabBarConfig_.style == "auto") return tabManager_->size() > 1;
        return true;
    }

    Tab* activeTab() { return tabManager_->activeTab(); }

    void notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn) {
        tabManager_->notifyAllTerminals(fn);
    }

    Terminal* activeTerm() { return tabManager_->activeTerm(); }

    // Shared rendering state
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
    bool              windowHasFocus_ = true;

    static std::string popupStateKey(int paneId, const std::string& popupId) {
        return std::to_string(paneId) + "/" + popupId;
    }

    // Owns the render worker thread, cross-thread mutation queues
    // (pending_, renderState_), the coarse mutex serializing render reads
    // vs main-thread structural mutations, and the deferred main/exit
    // post queues. Constructed early so collaborator Hosts can capture
    // its mutex()/pending()/renderState() references.
    std::unique_ptr<RenderThread> renderThread_;

    // References into the RenderThread's owned state. These let existing
    // call sites read `platformMutex_`, `pending_`, `renderState_` without
    // a sweep; the unique_ptr outlives every caller because it is reset
    // last in the destructor.
    std::mutex&        platformMutex_;
    PendingMutations&  pending_;
    RenderFrameState&  renderState_;

    // Build renderState_ from current tabs_/panes.  Called under the
    // render-thread mutex from RenderThread::applyPendingMutations(),
    // which also performs the pending_ flag transfer.
    void buildRenderFrameState();

    // Tab bar
    TabBarConfig  tabBarConfig_;
    std::string   tabBarFontName_   = "tab_bar";
    float         tabBarFontSize_   = 0.0f;
    float         tabBarCharWidth_  = 0.0f;
    float         tabBarLineHeight_ = 0.0f;
    int           tabBarCols_       = 0;
    bool          tabBarDirty_      = true;
    bool          dividersDirty_    = true;
    // Column ranges for each tab in the rendered tab bar, for hit-testing.
    // Each pair is (startCol, endCol) — the range of columns occupied by that tab.
    std::vector<std::pair<int,int>> tabBarColRanges_;
    int           tabBarAnimFrame_  = 0;
    uint64_t      lastAnimTick_     = 0;

    // Thin forwarders that preserve existing call-site ergonomics.
    void wakeRenderThread() { if (renderThread_) renderThread_->wake(); }
    void postToMainThread(std::function<void()> fn) {
        if (renderThread_) renderThread_->postToMain(std::move(fn));
    }
    void drainDeferredMain() { if (renderThread_) renderThread_->drainDeferredMain(); }
    void drainPendingExits() { if (renderThread_) renderThread_->drainPendingExits(); }
    void applyPendingMutations() { if (renderThread_) renderThread_->applyPendingMutations(); }

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

    void applyConfig(const Config& config);
    void applyFontChange(const Config& config);
    void invalidateAllRowCaches();

    // Owns keyboard / mouse input state and dispatch.
    std::unique_ptr<InputController> inputController_;

    void dispatchAction(const Action::Any& action);
    void executeAction(const Action::Any& action);

    // Typed dispatch entry. Owns the Action::Dispatcher listener registry
    // and the post-dispatch observer-notify + script-microtask flush.
    std::unique_ptr<ActionRouter> actionRouter_;

    // Script engine
    Script::Engine scriptEngine_;

    // Callback/terminal helpers — stays on PlatformDawn because it
    // closes over ~20 platform members (title/icon/cwd/progress/OSC/
    // foreground-process/mouse-cursor-shape) that TabManager doesn't
    // own. TabManager calls this via Host::buildTerminalCallbacks.
    TerminalCallbacks buildTerminalCallbacks(int paneId);
};

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, uint32_t flags = PlatformDawn::FlagNone);
