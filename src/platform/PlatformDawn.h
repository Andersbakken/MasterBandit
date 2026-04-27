#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <sys/types.h>
#include "ActionRouter.h"
#include "AnimationScheduler.h"
#include "ConfigLoader.h"
#include "Graveyard.h"
#include "InputController.h"
#include "InputTypes.h"
#include "ClickDetector.h"
#include "Terminal.h"
#include "TerminalOptions.h"
#include "TerminalSnapshot.h"
#include "Renderer.h"
#include "RenderEngine.h"
#include "RenderSync.h"
#include "RenderThread.h"
#include "TexturePool.h"
#include "text.h"
#include "DebugIPC.h"
#include "FontFallback.h"
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
//
// platformInit must run before any of the other platform calls. On Linux
// it opens the session-bus connection used for dark-mode discovery and
// notifications; on macOS it is currently a no-op. platformShutdown is the
// counterpart, called from PlatformDawn's destructor before the EventLoop
// itself is torn down.
void platformInit(EventLoop& loop);
void platformShutdown();
bool platformIsDarkMode();
void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback);
void platformInitNotifications();
void platformSetNotificationsShowWhenForeground(bool show);
// urgency: 0=low, 1=normal, 2=critical (freedesktop Notifications "urgency"
// hint). Currently honored on Linux; macOS impl ignores the value pending a
// per-platform mapping.
void platformSendNotification(const std::string& title, const std::string& body,
                              uint8_t urgency);
void platformOpenURL(const std::string& url);
std::string platformProcessCWD(pid_t pid);

#include "LayoutTree.h"
#include "Rect.h"
#include "Uuid.h"

class PlatformDawn {
    // InputController reaches into private members (renderThread_,
    // scriptEngine_, animScheduler_, fb/char/pad metrics, tabBarColRanges_).
    friend class InputController;
    // ActionRouter::dispatch wraps executeAction + scriptEngine_.executePendingJobs.
    friend class ActionRouter;
    // RenderThread drives onFrame on the render thread, buildRenderFrameState
    // under mutex, and calls terminalExited on drain.
    friend class RenderThread;
    // RenderEngine accesses textSystem_, renderThread_, animScheduler_,
    // debugIPC_ during renderFrame on the render thread.
    friend class RenderEngine;
    // ConfigLoader reads eventLoop_ for timer setup and calls applyConfig().
    friend class ConfigLoader;
    // AnimationScheduler reads eventLoop_ and calls onBlinkTick /
    // onResizeDebounceFire / setNeedsRedraw.
    friend class AnimationScheduler;

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
    void closeTab(Uuid subtreeRoot);

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

    bool shouldClose() {
        if (isHeadless()) return false;
        return window_ && window_->shouldClose();
    }

    std::string gridToJson(Uuid id);
    std::string statsJson(int requestId);

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

    // Script engine. Declared early so it outlives the PTY terminals and
    // layout tree during destruction — Terminals are owned by Engine and
    // hold Uuid handles into Engine::layoutTree().
    Script::Engine scriptEngine_;

    // --- Tab/pane primitives --------------------------------------------
    // Tab identity lives in the shared LayoutTree: each tab is a direct
    // child of Script::Engine::layoutRootStack_, identified by its
    // subtreeRoot Uuid. All tab-scoped methods take the subtreeRoot
    // directly — there is no separate Tab handle.
    //
    // The flat tab list, count, and active-tab index live on Script::Engine:
    // `scriptEngine().tabSubtreeRoots()`, `tabCount()`, `activeTabIndex()`.
    void setActiveTab(Uuid subtreeRoot);

    std::optional<Uuid> activeTab() const;
    std::optional<Uuid> tabAt(int idx) const;

    Terminal* activeTerm();

    void notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn);

    TerminalOptions& terminalOptions() { return terminalOptions_; }
    const TerminalOptions& terminalOptions() const { return terminalOptions_; }
    void setTerminalOptions(TerminalOptions opts) { terminalOptions_ = std::move(opts); }

    std::unordered_map<int, Terminal*>& ptyPolls() { return ptyPolls_; }
    void addPtyPoll(int fd, Terminal* term);
    void removePtyPoll(int fd);

    // Attach an orphan tab subtree (Stack) as a direct child of the root
    // Stack, optionally activating it.
    void attachLayoutSubtree(Uuid subtreeRoot, bool activate);

    // Allocate an empty tab subtree (Stack at the root, Container child).
    // Returns the new tab's subtreeRoot Uuid (nil on failure).
    Uuid createEmptyTab();
    void activateTabByUuid(Uuid subtreeRoot);

    bool createTerminalInContainer(Uuid parentContainerNodeId,
                                   const std::string& cwd,
                                   Uuid* outNodeId);
    bool splitPaneByNodeId(Uuid existingPaneNodeId, SplitDir dir,
                           float ratio, bool newIsFirst,
                           Uuid* outNodeId);
    bool removeNode(Uuid nodeId);
    bool focusPaneById(Uuid nodeId);

    // Walk pane/node membership against tab subtreeRoots; return the owning
    // tab's subtreeRoot Uuid, or nullopt if the node isn't under any tab.
    std::optional<Uuid> findTabForNode(Uuid nodeId) const;
    std::optional<Uuid> findTabForPane(Uuid nodeId) const;

    void terminalExited(Terminal* terminal);
    bool killTerminal(Uuid nodeId);

    void spawnTerminalForPane(Uuid nodeId, Uuid subtreeRoot, const std::string& cwd = {});
    void resizeAllPanesInTab(Uuid subtreeRoot);
    void refreshDividers(Uuid subtreeRoot);
    void clearDividers(Uuid subtreeRoot);
    void releaseTabTextures(Uuid subtreeRoot);

    void updateWindowTitle();
    void notifyPaneFocusChange(Uuid subtreeRoot, Uuid prevId, Uuid newId);


private:
    Uuid tabSubtreeRootAt(int idx) const;

    std::unordered_map<int, Terminal*> ptyPolls_;
    TerminalOptions                    terminalOptions_;

    void updateTabBarVisibility() {
        if (tabBarConfig_.style != "auto") return;
        bool visible = tabBarVisible();
        // Toggle the TabBar node's slot size in the tree. fixedCells=1 pins
        // the bar to one cell row. fixedCells=0 + stretch=0 makes the slot
        // collapse to zero (without the explicit stretch=0 the default
        // stretch=1 would split the parent container 50/50 with the
        // content slot — see setBarSlot in initTabBar).
        Uuid bar = scriptEngine_.primaryTabBarNode();
        if (!bar.isNil()) {
            LayoutTree& tree = scriptEngine_.layoutTree();
            if (const Node* n = tree.node(bar); n && !n->parent.isNil()) {
                tree.setSlotFixedCells(n->parent, bar, visible ? 1 : 0);
                tree.setSlotStretch(n->parent, bar, 0);
            }
        }
        // setSlotFixedCells marks the tree dirty; the per-frame gate in
        // buildRenderFrameState will run the resize cascade. Inline it here
        // too so the caller sees fresh rects synchronously (tests, callers
        // that read pane dims right after toggling bar visibility).
        runLayoutIfDirty();
        // Pull-model: tab-bar rendering pulls titles live from each tab's
        // remembered pane on every build, so no explicit refresh is needed
        // when the bar becomes visible.
    }

    bool tabBarVisible() const {
        if (tabBarConfig_.style == "hidden") return false;
        if (tabBarConfig_.style == "auto") return scriptEngine_.tabCount() > 1;
        return true;
    }

    static std::string popupStateKey(Uuid nodeId, const std::string& popupId) {
        return nodeId.toString() + "/" + popupId;
    }

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

    // Owns the render worker thread, coarse mutex, pending mutations,
    // renderState shadow copy, and the deferred main/exit queues.
    std::unique_ptr<RenderThread> renderThread_;

    // Deferred destruction of Terminal-owning objects. Entries are
    // stamped with the render thread's frame counter at stage time
    // (under renderThread_->mutex()) and freed on the main thread once
    // that counter has advanced — i.e. the render thread has finished
    // any frame that could have held a raw pointer to them.
    Graveyard graveyard_;

    // Build the renderState shadow copy from current tabs_/panes.
    // Called under the render-thread mutex from
    // RenderThread::applyPendingMutations(), which also transfers
    // pending flags into renderState.
    void buildRenderFrameState();

    // If the LayoutTree has been marked dirty since the last call, run a
    // full resize cascade across every tab (computeRects + TIOCSWINSZ +
    // divider refresh + render-state invalidation). Cheap when clean. Tree
    // mutations auto-mark via LayoutTree itself; callers who need fresh
    // rects synchronously (pane spawn/split before reading pane dims) also
    // call this.
    void runLayoutIfDirty();

    // Most-recent Config snapshot. Updated by applyConfig() on each
    // (re)load and read by the `mb.config` JS getter (via the configJson
    // AppCallbacks hook).
    Config        lastConfig_;

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

    // Pane divider colors
    float dividerR_ = 0.24f, dividerG_ = 0.24f, dividerB_ = 0.24f, dividerA_ = 1.0f;
    int   dividerWidth_ = 1;
    // OSC 133 selected-command outline (packed RGBA8, ABGR byte order for
    // compute shader unpacking).
    uint32_t commandOutlineColor_ = 0xFFAACCFFu;
    // OSC 133 non-selected row dim factor (0 = disabled).
    float commandDimFactor_ = 0.0f;
    // When true, Cmd+Up at oldest wraps to newest and vice versa; false clamps.
    bool commandNavigationWrap_ = true;

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

    // Build the TerminalCallbacks shim for a freshly-spawned Terminal. Lives
    // on PlatformDawn because it closes over ~20 members (title/icon/cwd/
    // progress/OSC/foreground-process/mouse-cursor-shape).
    TerminalCallbacks buildTerminalCallbacks(Uuid nodeId);

    // Render-thread hooks (called by RenderEngine::renderFrame). All of
    // these acquire renderThread_->mutex() internally.
    bool snapshotUnderLock(RenderFrameState& out);
    std::tuple<bool, uint32_t, uint32_t> takeSurfaceReconfigureRequest();
    bool takeViewportSizeChangedRequest();
    std::pair<uint32_t, uint32_t> currentFbSize();
};

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, uint32_t flags = PlatformDawn::FlagNone);
