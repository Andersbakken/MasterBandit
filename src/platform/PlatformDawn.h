#pragma once

#include "Action.h"
#include "ActionRouter.h"
#include "AnimationScheduler.h"
#include "Bindings.h"
#include "ClickDetector.h"
#include "ConfigLoader.h"
#include "DebugIPC.h"
#include "FontFallback.h"
#include "Graveyard.h"
#include "InputController.h"
#include "InputTypes.h"
#include "PtyMux.h"
#include "RenderEngine.h"
#include "RenderSync.h"
#include "RenderThread.h"
#include "Renderer.h"
#include "ScriptEngine.h"
#include "Terminal.h"
#include "TerminalOptions.h"
#include "TerminalSnapshot.h"
#include "TexturePool.h"
#include "WorkerPool.h"
#include "text.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <sys/types.h>

#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>

#include <eventloop/EventLoop.h>
#include <eventloop/Window.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Custom spdlog sink that forwards to DebugIPC
class DebugIPCSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
    void setIPC(DebugIPC *ipc) { ipc_ = ipc; }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        if (!ipc_) {
            return;
        }
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        ipc_->broadcastLog(std::string(formatted.data(), formatted.size()));
    }

    void flush_() override {}

private:
    DebugIPC *ipc_ = nullptr;
};

#include "PlatformFunctions.h"

#include "LayoutTree.h"
#include "Rect.h"
#include "Uuid.h"

class PlatformDawn
{
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
    // setNeedsRedraw.
    friend class AnimationScheduler;

public:
    enum Flag : uint32_t
    {
        FlagNone     = 0,
        FlagHeadless = 1 << 0,
        FlagIPC      = 1 << 1,
    };

    PlatformDawn(int argc, char **argv, uint32_t flags = FlagNone);
    ~PlatformDawn();

    int exec();
    void quit(int status = 0);
    void createTerminal(const TerminalOptions &options);
    void closeTab(Uuid subtreeRoot);

    bool isHeadless() const { return flags_ & FlagHeadless; }

    bool hasFlag(Flag f) const { return flags_ & f; }

    void setTestConfig(const std::string &fontPath, int cols, int rows, float fontSize,
                       const std::string &emojiFontPath    = {},
                       const std::string &fallbackFontPath = {})
    {
        testFontPath_         = fontPath;
        testCols_             = cols;
        testRows_             = rows;
        testFontSize_         = fontSize;
        testEmojiFontPath_    = emojiFontPath;
        testFallbackFontPath_ = fallbackFontPath;
    }

    const std::string &exeDir() const { return exeDir_; }

    void onFramebufferResize(int width, int height);
    void adjustFontSize(float delta);

    void setNeedsRedraw();

    bool shouldClose()
    {
        if (isHeadless()) {
            return false;
        }
        return window_ && window_->shouldClose();
    }

    std::string gridToJson(Uuid id);
    std::string statsJson(int requestId);

    // Resolves the effective dark-mode value for mode 2031 / DSR-997.
    // Honors the `color_scheme` config override ("light" / "dark") when
    // set; otherwise queries the system (DBus portal on Linux,
    // NSApp.effectiveAppearance on macOS).
    //
    // Recomputed only when state changes (config reload, OS appearance
    // notification) and cached in `cachedIsDarkMode_` so the parser worker
    // can read it via an atomic load without bouncing through runOnMain.
    // Bouncing while the parser holds Terminal::mutex() deadlocked against
    // the render thread (which also takes Terminal::mutex() via
    // forEachEmbedded in buildRenderFrameState).
    bool effectiveIsDarkMode() const;

    bool cachedIsDarkMode() const { return cachedIsDarkMode_.load(std::memory_order_acquire); }

    void refreshCachedIsDarkMode() { cachedIsDarkMode_.store(effectiveIsDarkMode(), std::memory_order_release); }

    // True iff called from the thread that constructed PlatformDawn (the
    // main / event-loop thread). Used by sync TerminalCallbacks shims so
    // they can call directly when invoked on the main thread and bounce
    // through eventLoop_->post when invoked from a parse-worker thread.
    bool isMainThread() const { return std::this_thread::get_id() == mainThreadId_; }

    // Run fn on the main thread and return its result. If already on the
    // main thread, runs synchronously. Otherwise posts to the event loop
    // and blocks until the post is drained. Used to back the sync
    // TerminalCallbacks (pasteFromClipboard, isDarkMode, customTcapLookup)
    // when injectData runs on a parse worker.
    template <typename Fn>
    auto runOnMain(Fn &&fn) -> std::invoke_result_t<Fn>
    {
        using R = std::invoke_result_t<Fn>;
        if (isMainThread()) {
            return fn();
        }
        std::promise<R> p;
        auto fut = p.get_future();
        eventLoop_->post([&p, &fn]() mutable
                         {
                             try {
                                 if constexpr (std::is_void_v<R>) {
                                     fn();
                                     p.set_value();
                                 } else {
                                     p.set_value(fn());
                                 }
                             } catch (...) {
                                 p.set_exception(std::current_exception());
                             }
                         });
        return fut.get();
    }

private:
    int exitStatus_ = 0;
    bool running_   = false;

    // Captured in the constructor (which runs on the main / event-loop
    // thread) so runOnMain() can detect re-entrant calls and avoid
    // deadlocking by posting to itself.
    std::thread::id mainThreadId_ { std::this_thread::get_id() };

    // Cached effectiveIsDarkMode() result, refreshed by main on config
    // reload and OS appearance changes; read by parser workers via
    // cachedIsDarkMode() to avoid a runOnMain bounce inside injectData.
    std::atomic<bool> cachedIsDarkMode_ { false };

    uint32_t flags_           = FlagNone;
    bool platformInitialized_ = false;
    int testCols_             = 80;
    int testRows_             = 24;
    float testFontSize_       = 16.0f;
    std::string testFontPath_;
    std::string testEmojiFontPath_;
    std::string testFallbackFontPath_;
    std::unique_ptr<EventLoop> eventLoop_;
    std::unique_ptr<Window> window_;
    // Owns the PTY-read polling thread + FdPoller. PTY reads happen
    // here, not on eventLoop_'s thread — see PTY_READER_REFACTOR.md.
    // Constructed in Platform_EventLoop.cpp::exec() after eventLoop_;
    // stopped + reset before eventLoop_ tears down.
    std::unique_ptr<PtyMux> ptyMux_;

    // Blink / animation wakeup / resize debounce timers live in
    // AnimationScheduler. Opacity is snapshotted into RenderFrameState.
    std::unique_ptr<AnimationScheduler> animScheduler_;
    // Config hot-reload: owns the file watcher debounce timer and the
    // reloadNow() entry point. Calls back into applyConfig() on success.
    std::unique_ptr<ConfigLoader> configLoader_;
    // Optional ~/.config/MasterBandit/config.js — fully-trusted JS
    // controller loaded after TOML, can override anything via
    // mb.config.patch / addKeybinding / etc. 0 if no config.js exists or
    // initial load failed (file-watch will retry on save).
    Script::InstanceId configJsInstanceId_    = 0;
    EventLoop::TimerId configJsDebounceTimer_ = 0;
    bool configJsDebounceActive_              = false;
    // Owns Dawn device/queue/surface, Renderer, TexturePool, and all
    // render-thread-only state (pane/popup/overlay private state, frameState_,
    // tab-bar GPU texture). Created early in ctor so accessors work.
    std::unique_ptr<RenderEngine> renderEngine_;
    void onBlinkTick();
    void applyFramebufferResize(int width, int height);
    void flushPendingFramebufferResize();
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
    std::optional<Uuid> activeTab() const;
    std::optional<Uuid> tabAt(int idx) const;

    Terminal *activeTerm();

    void notifyAllTerminals(const std::function<void(TerminalEmulator *)> &fn);

    TerminalOptions &terminalOptions() { return terminalOptions_; }

    const TerminalOptions &terminalOptions() const { return terminalOptions_; }

    void setTerminalOptions(TerminalOptions opts) { terminalOptions_ = std::move(opts); }

    std::unordered_map<int, Terminal *> &ptyPolls() { return ptyPolls_; }

    void addPtyPoll(int fd, Terminal *term);
    void removePtyPoll(int fd);

    // Attach an orphan tab subtree (Stack) as a direct child of the root
    // Stack, optionally activating it.
    void attachLayoutSubtree(Uuid subtreeRoot, bool activate);

    // Allocate an empty tab subtree (Stack at the root, Container child).
    // Returns the new tab's subtreeRoot Uuid (nil on failure).
    Uuid createEmptyTab();
    void activateTabByUuid(Uuid subtreeRoot);

    bool createTerminalInContainer(Uuid parentContainerNodeId,
                                   const std::string &cwd,
                                   Uuid *outNodeId);
    // `cwd` is the explicit caller-supplied working directory for the
    // new pane. If empty, falls back to paneProcessCWD(existingPane)
    // (OSC 7 → /proc) so callers that don't supply one (e.g. user
    // scripts that bypass default-ui) still get sensible inheritance.
    bool splitPaneByNodeId(Uuid existingPaneNodeId, SplitDir dir,
                           float ratio, bool newIsFirst,
                           const std::string &cwd,
                           Uuid *outNodeId);
    bool removeNode(Uuid nodeId);
    bool focusPaneById(Uuid nodeId);

    // Walk pane/node membership against tab subtreeRoots; return the owning
    // tab's subtreeRoot Uuid, or nullopt if the node isn't under any tab.
    std::optional<Uuid> findTabForNode(Uuid nodeId) const;
    std::optional<Uuid> findTabForPane(Uuid nodeId) const;

    void terminalExited(Terminal *terminal);
    bool killTerminal(Uuid nodeId);

    void spawnTerminalForPane(Uuid nodeId, Uuid subtreeRoot, const std::string &cwd = {});
    void resizeAllPanesInTab(Uuid subtreeRoot);
    void refreshDividers(Uuid subtreeRoot);
    void clearDividers(Uuid subtreeRoot);
    void releaseTabTextures(Uuid subtreeRoot);

    void updateWindowTitle();
    void notifyPaneFocusChange(Uuid subtreeRoot, Uuid prevId, Uuid newId);

private:
    Uuid tabSubtreeRootAt(int idx) const;

    std::unordered_map<int, Terminal *> ptyPolls_;
    TerminalOptions terminalOptions_;

    void updateTabBarVisibility()
    {
        if (tabBarConfig_.style != "auto") {
            return;
        }
        bool visible = tabBarVisible();
        // Toggle the TabBar node's slot size in the tree. fixedCells=1 pins
        // the bar to one cell row. fixedCells=0 + stretch=0 makes the slot
        // collapse to zero (without the explicit stretch=0 the default
        // stretch=1 would split the parent container 50/50 with the
        // content slot — see setBarSlot in initTabBar).
        Uuid bar     = scriptEngine_.primaryTabBarNode();
        if (!bar.isNil()) {
            LayoutTree &tree = scriptEngine_.layoutTree();
            if (const Node *n = tree.node(bar); n && !n->parent.isNil()) {
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

    bool tabBarVisible() const
    {
        if (tabBarConfig_.style == "hidden") {
            return false;
        }
        if (tabBarConfig_.style == "auto") {
            return scriptEngine_.tabCount() > 1;
        }
        return true;
    }

    static std::string popupStateKey(Uuid nodeId, const std::string &popupId)
    {
        return nodeId.toString() + "/" + popupId;
    }

    // Shared rendering state
    TextSystem textSystem_;
    std::string fontName_        = "mono";
    float fontSize_              = 16.0f;
    float baseFontSize_          = 0.0f;
    std::string replacementChar_ = "\xEF\xBF\xBD";
    float charWidth_             = 0.0f;
    float lineHeight_            = 0.0f;
    float contentScaleX_ = 1.0f, contentScaleY_ = 1.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;
    std::string primaryFontPath_;
    FontFallback fontFallback_;

    // Default colors
    uint32_t defaultFgColor_ = 0xFFDDDDDD;
    uint32_t defaultBgColor_ = 0x00000000;
    bool windowHasFocus_     = true;
    // Mirrors the windowing toolkit's visibility state — fed by
    // Window::onVisibility (xcb tracks mapped/iconified/fully-obscured).
    // Used together with windowHasFocus_ to drive OSC 99 `o=` gating via
    // platformSetNotificationWindowState. macOS reads NSWindow state
    // directly inside the platform layer and ignores this field.
    bool windowVisible_      = true;

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
    // Enumerate every visible TabBar in the layout and populate one
    // RenderFrameState::TabBarRender entry per bar (rect + per-tab
    // metadata + cell layout). Called from buildRenderFrameState; split
    // out so the per-bar sourcing rules (primary uses tabSubtreeRoots,
    // sub-bars walk their bound Stack's children) stay readable.
    void populateTabBars();

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
    Config lastConfig_;

    // Coalesced JS-driven config updates. Multiple `mb.config.addKeybinding`
    // / `removeKeybinding` / `patch` / `applyConfigJson` calls within a
    // single tick each stash the next draft here, but only ONE realize-pass
    // (applyConfig) is scheduled via eventLoop_->post. JS reads see the
    // pending draft (so subsequent patches see their predecessors).
    std::optional<Config> pendingConfigDraft_;
    bool pendingConfigScheduled_ = false;

    // Tab bar
    TabBarConfig tabBarConfig_;
    std::string tabBarFontName_ = "tab_bar";
    float tabBarFontSize_       = 0.0f;
    float tabBarCharWidth_      = 0.0f;
    float tabBarLineHeight_     = 0.0f;
    bool tabBarDirty_           = true;
    bool dividersDirty_         = true;
    int tabBarAnimFrame_        = 0;
    uint64_t lastAnimTick_      = 0;

    // Pane divider colors
    float dividerR_ = 0.24f, dividerG_ = 0.24f, dividerB_ = 0.24f, dividerA_ = 1.0f;
    int dividerWidth_             = 1;
    // OSC 133 selected-command outline (packed RGBA8, ABGR byte order for
    // compute shader unpacking).
    uint32_t commandOutlineColor_ = 0xFFAACCFFu;
    // OSC 133 non-selected row dim factor (0 = disabled).
    float commandDimFactor_       = 0.0f;
    // When true, Cmd+Up at oldest wraps to newest and vice versa; false clamps.
    bool commandNavigationWrap_   = true;

    // Pane tints
    float activeTint_[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float inactiveTint_[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Tab bar colors
    uint32_t tbBgColor_         = 0xFF261926;
    uint32_t tbActiveBgColor_   = 0xFFf7a27a;
    uint32_t tbActiveFgColor_   = 0xFF261a1b;
    uint32_t tbInactiveBgColor_ = 0xFF3b2824;
    uint32_t tbInactiveFgColor_ = 0xFF895f56;
    float progressColorR_ = 0.0f, progressColorG_ = 0.6f, progressColorB_ = 1.0f;
    float progressBarHeight_ = 3.0f;

    // Scaled padding in pixels
    float padLeft_ = 0, padTop_ = 0, padRight_ = 0, padBottom_ = 0;

    void initTabBar(const TabBarConfig &cfg);

    void applyConfig(const Config &config);
    // JS-driven entry point: stash draft + schedule a coalesced apply on the
    // next eventloop drain. Multiple calls in the same tick collapse into a
    // single realize-pass over the latest draft.
    void scheduleApplyConfig(Config draft);
    void applyFontChange(const Config &config);
    void invalidateAllRowCaches();

    // Owns keyboard / mouse input state and dispatch.
    std::unique_ptr<InputController> inputController_;

    void dispatchAction(const Action::Any &action);
    void executeAction(const Action::Any &action);

    // Typed dispatch entry. Owns the Action::Dispatcher listener registry
    // and the post-dispatch observer-notify + script-microtask flush.
    std::unique_ptr<ActionRouter> actionRouter_;

    // Build the TerminalCallbacks shim for a freshly-spawned Terminal. Lives
    // on PlatformDawn because it closes over ~20 members (title/icon/cwd/
    // progress/OSC/foreground-process/mouse-cursor-shape).
    TerminalCallbacks buildTerminalCallbacks(Uuid nodeId);

    // Render-thread hooks (called by RenderEngine::renderFrame). All of
    // these acquire renderThread_->mutex() internally.
    bool snapshotUnderLock(RenderFrameState &out);
    std::tuple<bool, uint32_t, uint32_t> takeSurfaceReconfigureRequest();
    bool takeViewportSizeChangedRequest();
    std::pair<uint32_t, uint32_t> currentFbSize();
};

std::unique_ptr<PlatformDawn> createPlatform(int argc, char **argv, uint32_t flags = PlatformDawn::FlagNone);
