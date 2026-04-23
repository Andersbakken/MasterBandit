#include "PlatformDawn.h"
#include "ActionRouter.h"
#include "AnimationScheduler.h"
#include "ConfigLoader.h"
#include "InputController.h"
#include "Utils.h"
#include "FontResolver.h"
#include "Utf8.h"

#ifdef __APPLE__
#  include <mac/EventLoop_nsapp.h>
#  include <mac/Window_cocoa.h>
#  include <kqueue/EventLoop_kqueue.h>
#elif defined(__linux__)
#  include <epoll/EventLoop_epoll.h>
#  include <xcb/Window_xcb.h>
#endif
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/ioctl.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

static std::vector<uint8_t> loadFontFile(const std::string& path) { return io::loadFile(path); }


PlatformDawn::PlatformDawn(int argc, char** argv, uint32_t flags)
    : flags_(flags)
    , renderThread_(std::make_unique<RenderThread>())
{
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/mb.log", true);
        auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        debugSink_ = std::make_shared<DebugIPCSink>();
        std::vector<spdlog::sink_ptr> sinks = {fileSink, stderrSink, debugSink_};
        auto logger = std::make_shared<spdlog::logger>("mb", sinks.begin(), sinks.end());
        logger->set_level(spdlog::default_logger()->level()); // inherit level set in main()
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
    } catch (...) {}

    exeDir_ = fs::weakly_canonical(fs::path(argv[0])).parent_path().string();

    renderEngine_ = std::make_unique<RenderEngine>();
    inputController_ = std::make_unique<InputController>();
    actionRouter_ = std::make_unique<ActionRouter>();
    {
        ActionRouter::Host rh;
        rh.executeAction = [this](const Action::Any& a) { executeAction(a); };
        rh.flushScriptJobs = [this]() { scriptEngine_.executePendingJobs(); };
        actionRouter_->setHost(std::move(rh));
    }
    configLoader_ = std::make_unique<ConfigLoader>();
    {
        InputController::Host ih;
        ih.platformMutex = &renderThread_->mutex();
        ih.headless = isHeadless();
        ih.activeTab = [this]() -> std::optional<Tab> { return activeTab(); };
        ih.activeTerm = [this]() -> TerminalEmulator* {
            return static_cast<TerminalEmulator*>(activeTerm());
        };
        ih.activeTabIdx = [this]() -> int { return activeTabIdx(); };
        ih.dispatchAction = [this](const Action::Any& a) { dispatchAction(a); };
        ih.setNeedsRedraw = [this]() { setNeedsRedraw(); };
        ih.resetBlink = [this]() {
            if (animScheduler_) animScheduler_->resetBlink();
        };
        ih.fbWidth = [this]() -> uint32_t { return fbWidth_; };
        ih.fbHeight = [this]() -> uint32_t { return fbHeight_; };
        ih.charWidth = [this]() -> float { return charWidth_; };
        ih.lineHeight = [this]() -> float { return lineHeight_; };
        ih.contentScaleX = [this]() -> float { return contentScaleX_; };
        ih.contentScaleY = [this]() -> float { return contentScaleY_; };
        ih.padLeft = [this]() -> float { return padLeft_; };
        ih.padTop = [this]() -> float { return padTop_; };
        ih.tabBarVisible = [this]() -> bool { return tabBarVisible(); };
        ih.tabBarCharWidth = [this]() -> float { return tabBarCharWidth_; };
        ih.tabBarColRanges = [this]() -> const std::vector<std::pair<int,int>>& {
            return tabBarColRanges_;
        };
        ih.notifyPaneFocusChange = [this](Tab t, Uuid prev, Uuid next) {
            notifyPaneFocusChange(t, prev, next);
        };
        ih.updateTabTitleFromFocusedPane = [this](int tabIdx) {
            updateTabTitleFromFocusedPane(tabIdx);
        };
        ih.scriptEngine = &scriptEngine_;
        ih.eventLoop = [this]() -> EventLoop* { return eventLoop_.get(); };
        ih.window = [this]() -> Window* { return window_.get(); };
        inputController_->setHost(std::move(ih));
    }

    RenderEngine::Host host;
    host.headless = isHeadless();
    host.textSystem = &textSystem_;
    host.snapshotUnderLock = [this](RenderFrameState& out) -> bool {
        std::lock_guard<std::recursive_mutex> lk(renderThread_->mutex());
        if (fbWidth_ == 0 || fbHeight_ == 0) return false;
        RenderFrameState& rs = renderThread_->renderState();
        out = rs;
        // Consume one-shot flags so the next frame doesn't re-process them.
        rs.focusChanged = false;
        rs.mainFontAtlasChanged = false;
        rs.tabBarFontAtlasChanged = false;
        rs.mainFontRemoved = false;
        rs.tabBarFontRemoved = false;
        rs.viewportSizeChanged = false;
        rs.tabBarDirty = false;
        rs.dividersDirty = false;
        rs.releasePaneTextureIds.clear();
        rs.releasePopupTextureKeys.clear();
        rs.releaseAllPaneTextures = false;
        rs.releaseTabBarTexture = false;
        rs.invalidateAllRowCaches = false;
        rs.destroyedPaneIds.clear();
        rs.destroyedPopupKeys.clear();
        return true;
    };
    host.takeSurfaceReconfigureRequest = [this]() -> std::tuple<bool, uint32_t, uint32_t> {
        std::lock_guard<std::recursive_mutex> lk(renderThread_->mutex());
        RenderFrameState& rs = renderThread_->renderState();
        if (rs.surfaceNeedsReconfigure) {
            rs.surfaceNeedsReconfigure = false;
            return {true, fbWidth_, fbHeight_};
        }
        return {false, 0u, 0u};
    };
    host.takeViewportSizeChangedRequest = [this]() -> bool {
        std::lock_guard<std::recursive_mutex> lk(renderThread_->mutex());
        RenderFrameState& rs = renderThread_->renderState();
        if (rs.viewportSizeChanged) {
            rs.viewportSizeChanged = false;
            return true;
        }
        return false;
    };
    host.currentFbSize = [this]() -> std::pair<uint32_t, uint32_t> {
        std::lock_guard<std::recursive_mutex> lk(renderThread_->mutex());
        return {fbWidth_, fbHeight_};
    };
    host.postToMain = [this](std::function<void()> fn) { renderThread_->postToMain(std::move(fn)); };
    host.scheduleAnimationAt = [this](uint64_t dueAtNs) {
        if (animScheduler_) animScheduler_->scheduleAnimationAt(dueAtNs);
    };
    host.debugIPC = [this]() -> DebugIPC* { return debugIPC_.get(); };
    host.notifyFrameCompleted = [this]() {
        if (renderThread_) renderThread_->notifyFrameCompleted();
    };
    renderEngine_->setHost(std::move(host));

    {
        RenderThread::Host rtHost;
        rtHost.eventLoop = [this]() -> EventLoop* { return eventLoop_.get(); };
        rtHost.onFrame = [this]() {
            if (!renderEngine_) return;
            // Tick Dawn on the render thread: drains device-side events
            // (completion callbacks, deferred destroys). Safe to run without
            // the render-thread mutex because all GPU calls happen on this
            // thread only.
            renderEngine_->device().Tick();
            // renderFrame manages the render-thread mutex internally via the
            // Host snapshot callback — it releases the lock during shaping
            // so input handlers / deferred callbacks on main thread can
            // proceed concurrently with the CPU-heavy section.
            renderEngine_->renderFrame();
        };
        rtHost.buildRenderFrameState = [this]() { buildRenderFrameState(); };
        rtHost.onTerminalExit = [this](Terminal* t) { terminalExited(t); };
        renderThread_->setHost(std::move(rtHost));
    }

    if (!renderEngine_->initGpu()) {
        // initGpu already logged
    }
}

void PlatformDawn::runLayoutIfDirty()
{
    if (!scriptEngine_.layoutTree().takeDirty()) return;
    for (Tab& t : tabs()) {
        if (t.valid()) resizeAllPanesInTab(t);
    }
}

void PlatformDawn::buildRenderFrameState()
{
    // If any tree mutations happened since the last frame (splits, zooms,
    // tab switches, etc.) run the resize cascade now. Single gate per frame:
    // the tree's dirty flag is cleared on consume, so subsequent calls are
    // no-ops until the next mutation. Called under the render mutex which
    // matches the locking order resizeAllPanesInTab expects.
    runLayoutIfDirty();

    // Called under the render-thread mutex. Rebuilds the shadow copy from
    // live state and merges main-thread-owned dirty flags (tabBarDirty_,
    // dividersDirty_) into renderThread_->renderState().
    renderThread_->renderState().tabBarDirty   |= tabBarDirty_;
    renderThread_->renderState().dividersDirty |= dividersDirty_;
    tabBarDirty_   = false;
    dividersDirty_ = false;
    auto tab = activeTab();
    renderThread_->renderState().activeTabIdx = activeTabIdx();
    renderThread_->renderState().fbWidth  = fbWidth_;
    renderThread_->renderState().fbHeight = fbHeight_;
    renderThread_->renderState().charWidth  = charWidth_;
    renderThread_->renderState().lineHeight = lineHeight_;
    renderThread_->renderState().fontSize   = fontSize_;
    renderThread_->renderState().padLeft   = padLeft_;
    renderThread_->renderState().padTop    = padTop_;
    renderThread_->renderState().padRight  = padRight_;
    renderThread_->renderState().padBottom = padBottom_;
    std::memcpy(renderThread_->renderState().activeTint,   activeTint_,   sizeof(activeTint_));
    std::memcpy(renderThread_->renderState().inactiveTint, inactiveTint_, sizeof(inactiveTint_));
    renderThread_->renderState().tabBarVisible = tabBarVisible();
    renderThread_->renderState().tabBarPosition = tabBarConfig_.position;
    renderThread_->renderState().inLiveResize = window_ && window_->inLiveResize();
    renderThread_->renderState().windowHasFocus = windowHasFocus_;
    renderThread_->renderState().cursorBlinkOpacity = animScheduler_ ? animScheduler_->blinkOpacity() : 1.0f;

    // Tab bar config values
    renderThread_->renderState().tbBgColor         = tbBgColor_;
    renderThread_->renderState().tbActiveBgColor   = tbActiveBgColor_;
    renderThread_->renderState().tbActiveFgColor   = tbActiveFgColor_;
    renderThread_->renderState().tbInactiveBgColor = tbInactiveBgColor_;
    renderThread_->renderState().tbInactiveFgColor = tbInactiveFgColor_;
    renderThread_->renderState().progressColorR    = progressColorR_;
    renderThread_->renderState().progressColorG    = progressColorG_;
    renderThread_->renderState().progressColorB    = progressColorB_;
    renderThread_->renderState().progressBarHeight = progressBarHeight_;
    renderThread_->renderState().progressBarEnabled = tabBarConfig_.progress_bar;
    renderThread_->renderState().progressIconEnabled = tabBarConfig_.progress_icon;
    renderThread_->renderState().maxTitleLength     = tabBarConfig_.max_title_length;

    // Divider appearance
    renderThread_->renderState().dividerWidth = dividerWidth_;
    renderThread_->renderState().dividerR = dividerR_;
    renderThread_->renderState().dividerG = dividerG_;
    renderThread_->renderState().dividerB = dividerB_;
    renderThread_->renderState().dividerA = dividerA_;
    renderThread_->renderState().commandOutlineColor = commandOutlineColor_;
    renderThread_->renderState().commandDimFactor = commandDimFactor_;

    // Font names and tab bar font metrics
    renderThread_->renderState().fontName = fontName_;
    renderThread_->renderState().tabBarFontName = tabBarFontName_;
    renderThread_->renderState().tabBarFontSize = tabBarFontSize_;
    renderThread_->renderState().tabBarCharWidth = tabBarCharWidth_;
    renderThread_->renderState().tabBarLineHeight = tabBarLineHeight_;

    // Content scale
    renderThread_->renderState().contentScaleX = contentScaleX_;

    // Tab bar animation
    renderThread_->renderState().tabBarAnimFrame = tabBarAnimFrame_;

    // Tab bar rect from active tab's layout
    if (tab) {
        renderThread_->renderState().tabBarRect = tab->tabBarRect(fbWidth_, fbHeight_);
    } else {
        renderThread_->renderState().tabBarRect = {};
    }

    // Active tab pane info. `activePanes()` walks the tab's subtree honouring
    // Stack activeChild semantics — inactive Stack siblings (e.g. the content
    // Container while a scrollback pager is on top) don't contribute.
    renderThread_->renderState().panes.clear();
    renderThread_->renderState().focusedPaneId = {};

    if (tab) {
        renderThread_->renderState().focusedPaneId = tab->focusedPaneId();

        for (Terminal* pane : tab->activePanes()) {
            RenderPaneInfo rpi;
            rpi.id = pane->nodeId();
            rpi.rect = pane->rect();
            rpi.term = pane;
            rpi.progressState = pane->progressState();
            rpi.progressPct = pane->progressPct();
            rpi.focusedPopupId = pane->focusedPopupId();
            for (const auto& popup : pane->popups()) {
                RenderPanePopupInfo pi;
                pi.id = popup->popupId();
                pi.cellX = popup->cellX();
                pi.cellY = popup->cellY();
                pi.cellW = popup->cellW();
                pi.cellH = popup->cellH();
                pi.term = popup.get();
                rpi.popups.push_back(std::move(pi));
            }
            renderThread_->renderState().panes.push_back(std::move(rpi));
        }
    }

    // Tab bar data (all tabs)
    renderThread_->renderState().tabs.clear();
    auto allTabs = tabs();
    for (Tab& t : allTabs) {
        RenderTabInfo rti;
        rti.title = t.title();
        rti.icon  = t.icon();
        rti.focusedPaneId = t.valid() ? t.focusedPaneId() : Uuid{};
        Terminal* fp = t.valid() ? t.focusedPane() : nullptr;
        rti.progressState = fp ? fp->progressState() : 0;
        rti.progressPct   = fp ? fp->progressPct() : 0;
        renderThread_->renderState().tabs.push_back(std::move(rti));
    }


    // --- Tab bar layout (text → cell computation) ---
    renderThread_->renderState().tabBarCells.clear();
    renderThread_->renderState().tabBarColRanges.clear();
    renderThread_->renderState().tabBarCols = 0;

    if (!renderThread_->renderState().tabBarVisible || renderThread_->renderState().tabs.empty() ||
        renderThread_->renderState().tabBarRect.isEmpty() || renderThread_->renderState().tabBarCharWidth <= 0) {
        return;
    }

    int cols = std::max(1, static_cast<int>(renderThread_->renderState().tabBarRect.w / renderThread_->renderState().tabBarCharWidth));
    renderThread_->renderState().tabBarCols = cols;

    // Also update the main-thread-owned members used by resolveTabBarClickIndex
    tabBarCols_ = cols;

    // Helpers
    auto cpLen = [](const std::string& s) -> int {
        int w = 0;
        const char* p = s.c_str();
        while (*p) { p += utf8::seqLen(static_cast<uint8_t>(*p)); w++; }
        return w;
    };
    auto truncUtf8 = [](const std::string& s, int maxCp) -> std::string {
        if (maxCp <= 0) return {};
        int cp = 0;
        const char* p = s.c_str();
        while (*p && cp < maxCp) { p += utf8::seqLen(static_cast<uint8_t>(*p)); cp++; }
        if (*p) return std::string(s.c_str(), p) + "\xe2\x80\xa6";
        return s;
    };

    // Indeterminate animation glyphs
    static const char32_t kAnimGlyphs[] = {
        0xf0130, 0xf0a9e, 0xf0a9f, 0xf0aa0, 0xf0aa1,
        0xf0aa2, 0xf0aa3, 0xf0aa4, 0xf0aa5
    };
    static constexpr int kAnimCount = 9;
    auto cp32ToUtf8 = [](char32_t cp) -> std::string {
        std::string s;
        if (cp < 0x80) { s += static_cast<char>(cp); }
        else if (cp < 0x800) { s += static_cast<char>(0xC0|(cp>>6)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { s += static_cast<char>(0xE0|(cp>>12)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else { s += static_cast<char>(0xF0|(cp>>18)); s += static_cast<char>(0x80|((cp>>12)&0x3F)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        return s;
    };
    auto progressGlyph = [&](const RenderTabInfo& rti) -> std::string {
        int st = rti.progressState;
        int pct = rti.progressPct;
        if (st == 0) return "";
        int idx;
        if (st == 3) {
            int period = 2 * (kAnimCount - 1);
            int pos = renderThread_->renderState().tabBarAnimFrame % period;
            idx = (pos < kAnimCount) ? pos : period - pos;
        } else if (st == 1 || st == 2) {
            idx = std::clamp(pct * kAnimCount / 100, 0, kAnimCount - 1);
        } else return "";
        return cp32ToUtf8(kAnimGlyphs[idx]);
    };

    // Build per-tab info
    struct TI { std::string prefix; std::string title; std::string text; int width; bool isActive; uint32_t bgColor, fgColor; };
    std::vector<TI> tabInfos;
    for (int i = 0; i < static_cast<int>(renderThread_->renderState().tabs.size()); ++i) {
        const auto& rtab = renderThread_->renderState().tabs[i];
        bool isActive = (i == renderThread_->renderState().activeTabIdx);
        TI ti;
        ti.isActive = isActive;
        ti.bgColor = isActive ? renderThread_->renderState().tbActiveBgColor : renderThread_->renderState().tbInactiveBgColor;
        ti.fgColor = isActive ? renderThread_->renderState().tbActiveFgColor : renderThread_->renderState().tbInactiveFgColor;
        ti.prefix = " ";
        std::string pg = renderThread_->renderState().progressIconEnabled ? progressGlyph(rtab) : "";
        if (!pg.empty()) { ti.prefix += pg; ti.prefix += " "; }
        if (!rtab.icon.empty()) { ti.prefix += rtab.icon; ti.prefix += " "; }
        ti.prefix += "["; ti.prefix += std::to_string(i + 1); ti.prefix += "] ";
        ti.title = rtab.title;
        tabInfos.push_back(std::move(ti));
    }

    int numTabs = static_cast<int>(tabInfos.size());
    int sepWidth = 1;

    // Truncation loop
    int maxTitleLen = renderThread_->renderState().maxTitleLength > 0 ? renderThread_->renderState().maxTitleLength : 9999;
    for (;;) {
        int total = 0;
        for (int i = 0; i < numTabs; ++i) {
            std::string truncTitle = truncUtf8(tabInfos[i].title, maxTitleLen);
            tabInfos[i].text = tabInfos[i].prefix + truncTitle + (truncTitle.empty() ? "" : " ");
            if (tabInfos[i].text.back() != ' ') tabInfos[i].text += " ";
            tabInfos[i].width = cpLen(tabInfos[i].text);
            total += tabInfos[i].width + sepWidth;
        }
        if (total <= cols || maxTitleLen <= 0) break;
        maxTitleLen--;
    }

    // Overflow detection
    int totalWidth = 0;
    for (auto& ti : tabInfos) totalWidth += ti.width + sepWidth;
    int visStart = 0, visEnd = numTabs;
    bool overflowLeft = false, overflowRight = false;
    if (totalWidth > cols && numTabs > 1) {
        int ati = renderThread_->renderState().activeTabIdx;
        if (ati < 0 || ati >= numTabs) ati = 0;
        visStart = ati; visEnd = ati + 1;
        int used = tabInfos[ati].width + sepWidth;
        int indicatorWidth = 2;
        while (visStart > 0 || visEnd < numTabs) {
            bool expanded = false;
            if (visEnd < numTabs) {
                int need = tabInfos[visEnd].width + sepWidth + (visEnd + 1 < numTabs ? indicatorWidth : 0);
                if (used + need + (visStart > 0 ? indicatorWidth : 0) <= cols) {
                    used += tabInfos[visEnd].width + sepWidth; visEnd++; expanded = true;
                }
            }
            if (visStart > 0) {
                int need = tabInfos[visStart - 1].width + sepWidth + (visStart - 1 > 0 ? indicatorWidth : 0);
                if (used + need + (visEnd < numTabs ? indicatorWidth : 0) <= cols) {
                    visStart--; used += tabInfos[visStart].width + sepWidth; expanded = true;
                }
            }
            if (!expanded) break;
        }
        overflowLeft = (visStart > 0);
        overflowRight = (visEnd < numTabs);
    }

    // Build cell array
    renderThread_->renderState().tabBarCells.resize(cols);
    for (auto& c : renderThread_->renderState().tabBarCells) {
        c.ch.clear();
        c.fgColor = renderThread_->renderState().tbInactiveFgColor;
        c.bgColor = renderThread_->renderState().tbBgColor;
    }

    auto placeCell = [&](int& col, const std::string& ch, uint32_t fg, uint32_t bg) {
        if (col >= cols) return;
        renderThread_->renderState().tabBarCells[col] = {ch, fg, bg};
        col++;
    };

    int col = 0;
    if (overflowLeft) {
        placeCell(col, "\xe2\x97\x80", renderThread_->renderState().tbInactiveFgColor, renderThread_->renderState().tbBgColor);
        placeCell(col, " ", renderThread_->renderState().tbInactiveFgColor, renderThread_->renderState().tbBgColor);
    }

    // Powerline separator
    const std::string SEP_RIGHT = "\xee\x82\xb0";

    renderThread_->renderState().tabBarColRanges.assign(renderThread_->renderState().tabs.size(), {-1, -1});
    for (int i = visStart; i < visEnd; ++i) {
        auto& ti = tabInfos[i];
        int startCol = col;
        const char* p = ti.text.c_str();
        while (*p && col < cols) {
            int len = utf8::seqLen(static_cast<uint8_t>(*p));
            std::string ch(p, static_cast<size_t>(len));
            placeCell(col, ch, ti.fgColor, ti.bgColor);
            p += len;
        }
        uint32_t nextBg = (i + 1 < visEnd) ? tabInfos[i + 1].bgColor : renderThread_->renderState().tbBgColor;
        placeCell(col, SEP_RIGHT, ti.bgColor, nextBg);
        renderThread_->renderState().tabBarColRanges[i] = {startCol, col};
    }

    // Also update main-thread-owned col ranges for click handling
    tabBarColRanges_ = renderThread_->renderState().tabBarColRanges;

    if (overflowRight && col + 2 <= cols) {
        placeCell(col, " ", renderThread_->renderState().tbInactiveFgColor, renderThread_->renderState().tbBgColor);
        placeCell(col, "\xe2\x96\xb6", renderThread_->renderState().tbInactiveFgColor, renderThread_->renderState().tbBgColor);
    }
}

PlatformDawn::~PlatformDawn()
{
    // Stop the render thread before destroying any Dawn / window state it
    // might be reading. RenderThread::stop() signals stop, wakes the
    // thread if idle, and joins.
    if (renderThread_) renderThread_->stop();

    // Detach window callbacks before tearing down components they reference.
    // Cocoa's -[NSWindow _close] fires windowDidResignKey synchronously,
    // which routes through onFocus → activeTab(); with the callbacks
    // cleared those spurious events become no-ops.
    if (window_) {
        window_->onKey = nullptr;
        window_->onChar = nullptr;
        window_->onFramebufferResize = nullptr;
        window_->onContentScale = nullptr;
        window_->onMouseButton = nullptr;
        window_->onCursorPos = nullptr;
        window_->onScroll = nullptr;
        window_->onExpose = nullptr;
        window_->onLiveResizeEnd = nullptr;
        window_->onFocus = nullptr;
    }

    // Render thread is joined; anything still deferred can now run its
    // destructors on the main thread with no concurrent access.
    graveyard_.drainAll();

    // Terminals live on Script::Engine now; its member-destruction order
    // (tab overlays → tab layouts → Terminals → layoutTree) runs when
    // scriptEngine_ goes out of scope below, after GPU resources.

    if (renderEngine_) {
        // Destroy the Vulkan surface and device, but intentionally leave the
        // Vulkan instance alive (nativeInstance_ is not reset inside shutdown()).
        renderEngine_->shutdown();
    }

    // Destroy the window (X11 connection) next:
    //  • The display must be alive during Vulkan device teardown (NVIDIA).
    //  • The Vulkan instance must be alive during XCloseDisplay (NVIDIA):
    //    vkDestroyInstance removes DRI/GLX hooks registered with Xlib;
    //    if they're gone before XCloseDisplay fires them it segfaults.
    // Both constraints are satisfied by this ordering.
    if (window_) {
        window_->destroy();
        window_.reset();
    }

    // Now that XCloseDisplay has run, drop the Vulkan instance.
    renderEngine_.reset();

    // Now safe to destroy the render thread component itself; the
    // worker thread has already been joined above.
    renderThread_.reset();
}

void PlatformDawn::createTerminal(const TerminalOptions& options)
{
    if (!renderEngine_ || !renderEngine_->device()) return;

    // One-time platform initialization (window/surface in windowed mode, font/renderer in both)
    if (!platformInitialized_) {
        platformInitialized_ = true;

        if (!isHeadless()) {
            // --- Windowed: create EventLoop + Window ---
#ifdef __APPLE__
            eventLoop_ = std::make_unique<NSAppEventLoop>();
            window_    = std::make_unique<CocoaWindow>();
#elif defined(__linux__)
            eventLoop_ = std::make_unique<EpollEventLoop>();
            window_.reset(new XCBWindow(*eventLoop_));
#endif

            // Wire up Window callbacks
            window_->onKey = [this](int key, int scancode, int action, int mods) {
                inputController_->onKey(key, scancode, action, mods);
            };
            window_->onChar = [this](uint32_t cp) { inputController_->onChar(cp); };
            window_->onFramebufferResize = [this](int w, int h) { onFramebufferResize(w, h); };
            window_->onContentScale = [this](float sx, float sy) {
                contentScaleX_ = sx; contentScaleY_ = sy;
            };
            window_->onMouseButton = [this](int button, int action, int mods) {
                inputController_->onMouseButton(button, action, mods);
            };
            window_->onCursorPos = [this](double x, double y) { inputController_->onCursorPos(x, y); };
            window_->onScroll = [this](double /*dx*/, double dy) {
                if (dy == 0) return;
                // If the active terminal has mouse reporting, send wheel
                // events to the application instead of scrolling the viewport.
                std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                auto tab = activeTab();
                if (tab) {
                    TerminalEmulator* term = nullptr;
                    Terminal* fp = tab->focusedPane();
                    term = fp;
                    if (term && term->mouseReportingActive()) {
                        PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
                        double lastCursorX = inputController_ ? inputController_->lastCursorX() : 0.0;
                        double lastCursorY = inputController_ ? inputController_->lastCursorY() : 0.0;
                        uint32_t lastMods = inputController_ ? inputController_->lastMods() : 0u;
                        double relX = lastCursorX - pr.x;
                        double relY = lastCursorY - pr.y;
                        MouseEvent mev;
                        mev.x = static_cast<int>(relX / charWidth_);
                        mev.y = static_cast<int>(relY / lineHeight_);
                        mev.globalX = static_cast<int>(lastCursorX);
                        mev.globalY = static_cast<int>(lastCursorY);
                        mev.pixelX = static_cast<int>(relX);
                        mev.pixelY = static_cast<int>(relY);
                        mev.button = (dy > 0) ? WheelUp : WheelDown;
                        mev.modifiers = lastMods;
                        term->mousePressEvent(&mev);
                        return;
                    }
                }
                if (dy > 0) dispatchAction(Action::ScrollUp{});
                else if (dy < 0) dispatchAction(Action::ScrollDown{});
            };
            window_->onExpose = [this]() { setNeedsRedraw(); };
            window_->onLiveResizeEnd = [this]() {
                // Apply any debounced resize captured during the drag so the
                // final frame reflects the true window geometry.
                {
                    std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                    flushPendingFramebufferResize();
                }
                setNeedsRedraw();
            };
            window_->onFocus = [this](bool focused) {
                windowHasFocus_ = focused;
                renderThread_->pending().focusChanged = true;
                setNeedsRedraw();
                auto t = activeTab();
                if (!t) return;
                if (t->valid()) {
                    if (Terminal* fp = t->focusedPane()) fp->focusEvent(focused);
                }
                setNeedsRedraw();
            };

            if (!window_->create(800, 600, "MasterBandit")) {
                spdlog::error("Failed to create window");
                return;
            }

            int w = 0, h = 0;
            window_->getFramebufferSize(w, h);
            fbWidth_  = static_cast<uint32_t>(w);
            fbHeight_ = static_cast<uint32_t>(h);

            float xscale = 1.0f, yscale = 1.0f;
            window_->getContentScale(xscale, yscale);
            contentScaleX_ = xscale;
            contentScaleY_ = yscale;
            fontSize_ = options.fontSize * xscale;
            baseFontSize_ = fontSize_;

            if (!renderEngine_->createSurface(window_.get())) {
                spdlog::error("Failed to create Dawn surface");
                return;
            }
            renderEngine_->configureSurface(fbWidth_, fbHeight_);

            // Scale texture pool limit based on screen resolution.
            // A full-screen RGBA texture is w*h*4 bytes; allow 4x that for
            // comfortable pooling during resizes and multi-pane layouts.
            {
                int sw = 0, sh = 0;
                window_->getScreenSize(sw, sh);
                constexpr size_t kMinLimit = 128 * 1024 * 1024;
                constexpr size_t kMaxLimit = 512 * 1024 * 1024;
                size_t limit = kMinLimit;
                if (sw > 0 && sh > 0) {
                    size_t screenBytes = static_cast<size_t>(sw) * sh * 4;
                    limit = std::clamp(screenBytes * 4, kMinLimit, kMaxLimit);
                }
                renderEngine_->texturePool().setByteLimit(limit);
                spdlog::info("TexturePool: screen {}x{}, limit set to {:.0f} MB",
                             sw, sh, limit / (1024.0 * 1024.0));
            }

            // Observe system appearance changes for mode 2031
            platformObserveAppearanceChanges([this](bool isDark) {
                notifyAllTerminals([isDark](TerminalEmulator* term) {
                    term->notifyColorPreference(isDark);
                });
            });
        } else {
            // --- Headless: event loop but no window/surface ---
#ifdef __APPLE__
            eventLoop_ = std::make_unique<KQueueEventLoop>();
#elif defined(__linux__)
            eventLoop_ = std::make_unique<EpollEventLoop>();
#endif
            contentScaleX_ = contentScaleY_ = 1.0f;
            fontSize_ = testFontSize_;
            baseFontSize_ = fontSize_;
        }

        {
            AnimationScheduler::Host h;
            h.eventLoop = eventLoop_.get();
            h.onRedraw = [this]() { setNeedsRedraw(); };
            h.onBlinkTick = [this]() { onBlinkTick(); };
            h.onResizeDebounceFire = [this](uint32_t w, uint32_t rh) {
                onResizeDebounceFire(w, rh);
            };
            animScheduler_ = std::make_unique<AnimationScheduler>(std::move(h));
        }

        // Load font
        std::string fontPath;
        if (isHeadless() && !testFontPath_.empty()) {
            fontPath = testFontPath_;
        } else if (options.font.empty()) {
            fontPath = resolveFontFamily("monospace");
        } else {
            fontPath = resolveFontFamily(options.font);
            if (fontPath.empty()) {
                spdlog::warn("Font family '{}' not found, falling back to system monospace", options.font);
                fontPath = resolveFontFamily("monospace");
            }
        }
        if (fontPath.empty()) {
            spdlog::error("No monospace font found on system");
            return;
        }
        spdlog::info("Using font: {}", fontPath);
        primaryFontPath_ = fontPath;

        auto fontData = loadFontFile(fontPath);
        if (fontData.empty()) {
            spdlog::error("Failed to load font: {}", fontPath);
            return;
        }

        std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};

        if (!isHeadless()) {
            const std::string& family = options.font.empty() ? std::string{} : options.font;

            // Load bold variant
            bool hasBoldFont = false;
            if (!family.empty()) {
                std::string boldPath = resolveFontFamily(family, FontTraitBold);
                if (!boldPath.empty() && boldPath != fontPath) {
                    auto boldData = loadFontFile(boldPath);
                    if (!boldData.empty()) {
                        spdlog::info("Using bold font: {}", boldPath);
                        fontList.push_back(std::move(boldData));
                        hasBoldFont = true;
                    }
                }
            }

            // Load italic variant
            bool hasItalicFont = false;
            uint32_t italicFontIndex = 0;
            if (!family.empty()) {
                std::string italicPath = resolveFontFamily(family, FontTraitItalic);
                if (!italicPath.empty() && italicPath != fontPath) {
                    auto italicData = loadFontFile(italicPath);
                    if (!italicData.empty()) {
                        spdlog::info("Using italic font: {}", italicPath);
                        italicFontIndex = static_cast<uint32_t>(fontList.size());
                        fontList.push_back(std::move(italicData));
                        hasItalicFont = true;
                    }
                }
            }

            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.setSystemFallback([this](const std::string& path, char32_t cp) {
                return fontFallback_.fontDataForCodepoint(path, cp);
            });
            textSystem_.setEmojiFallback([this](char32_t cp) {
                return fontFallback_.fontDataForEmoji(cp);
            });

            if (!hasBoldFont) {
                textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            }
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);

            if (hasItalicFont) {
                textSystem_.tagFontStyle(fontName_, italicFontIndex, {.bold = false, .italic = true});
            } else {
                textSystem_.addSyntheticItalicVariant(fontName_);
            }

            // Load bundled Symbols Nerd Font Mono as a built-in fallback
            auto nerdFontPath = fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf";
            auto nerdFontData = loadFontFile(nerdFontPath.string());
            if (!nerdFontData.empty()) {
                textSystem_.addFallbackFont(fontName_, nerdFontData);
                spdlog::info("Loaded built-in Symbols Nerd Font Mono");
            } else {
                spdlog::warn("Built-in Symbols Nerd Font Mono not found at {}", nerdFontPath.string());
            }

            // Load Noto Color Emoji if available (COLRv1 color emoji support).
            // Loaded before system fallback so it takes priority over Apple Color Emoji (sbix).
            auto notoEmojiPath = std::string(getenv("HOME") ? getenv("HOME") : "") + "/Library/Fonts/NotoColorEmoji-Regular.ttf";
            auto notoEmojiData = loadFontFile(notoEmojiPath);
            if (!notoEmojiData.empty()) {
                textSystem_.addFallbackFont(fontName_, notoEmojiData);
                spdlog::info("COLR: loaded Noto Color Emoji from {}", notoEmojiPath);
            }
        } else {
            // Headless: register font, optional emoji fallback for rendering tests
            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);
            if (!testFallbackFontPath_.empty()) {
                auto fallbackData = loadFontFile(testFallbackFontPath_);
                if (!fallbackData.empty()) {
                    textSystem_.addFallbackFont(fontName_, fallbackData);
                    spdlog::info("Test: loaded fallback font from {}", testFallbackFontPath_);
                }
            }
            if (!testEmojiFontPath_.empty()) {
                auto emojiData = loadFontFile(testEmojiFontPath_);
                if (!emojiData.empty()) {
                    textSystem_.addFallbackFont(fontName_, emojiData);
                    textSystem_.setEmojiFallback([emojiData](char32_t) { return emojiData; });
                    spdlog::info("Test: loaded emoji font from {}", testEmojiFontPath_);
                }
            }
        }

        const FontData* font = textSystem_.getFont(fontName_);
        if (!font) {
            spdlog::error("Failed to register font");
            return;
        }

        float scale = fontSize_ / font->baseSize;
        lineHeight_ = font->lineHeight * scale;

        const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
        charWidth_ = shaped.width;
        if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

        spdlog::info("Font metrics: charWidth={:.1f}, lineHeight={:.1f}", charWidth_, lineHeight_);

        // In headless mode, compute framebuffer size from target cols/rows
        if (isHeadless()) {
            fbWidth_ = static_cast<uint32_t>(std::ceil(testCols_ * charWidth_));
            fbHeight_ = static_cast<uint32_t>(std::ceil(testRows_ * lineHeight_));
            spdlog::info("Headless framebuffer: {}x{} ({}x{} cells)", fbWidth_, fbHeight_, testCols_, testRows_);
        }

        std::string shaderDir = fs::weakly_canonical(
            fs::path(exeDir_) / "shaders").string();
        if (!fs::exists(shaderDir)) {
            shaderDir = (fs::path(__FILE__).parent_path().parent_path() / "shaders").string();
        }
        renderEngine_->initRenderer(shaderDir, fbWidth_, fbHeight_);
        renderEngine_->uploadFontAtlas(fontName_, *font);

        // Spawn the render thread once all Dawn/surface/font state is
        // initialized. It sits idle on the condition variable until the main
        // thread signals a wake after parse/input/animation mutations.
        renderThread_->start();
    }

    // --- Apply options: padding, colors, bindings, dividers, tab bar ---
    // (The first tab + first Terminal are NOT built here. default-ui.js
    // constructs the tree in exec(): root Container with TabBar + tabs
    // Stack, first tab Stack, content Container, then calls
    // mb.layout.createTerminal to spawn the initial Terminal. This matches
    // the rule "native knows only nodes; JS places them".)
    setTerminalOptions(options);

    padLeft_   = options.padding.left   * contentScaleX_;
    padTop_    = options.padding.top    * contentScaleX_;
    padRight_  = options.padding.right  * contentScaleX_;
    padBottom_ = options.padding.bottom * contentScaleX_;

    const auto& cs = options.colors;
    defaultFgColor_ = color::parseHexRGBA(cs.foreground, 0xFFDDDDDD);
    defaultBgColor_ = color::parseHexRGBA(cs.background, 0x00000000);

    {
        std::vector<Binding> all = defaultBindings();
        auto userBindings = parseBindings(options.keybindings);
        all.insert(all.end(), userBindings.begin(), userBindings.end());
        inputController_->setKeyBindings(std::move(all));
    }
    {
        std::vector<MouseBinding> all = defaultMouseBindings();
        auto userMouseBindings = parseMouseBindings(options.mousebindings);
        all.insert(all.end(), userMouseBindings.begin(), userMouseBindings.end());
        inputController_->setMouseBindings(std::move(all));
    }

    dividerWidth_ = std::max(0, options.dividerWidth);
    const std::string& dc = options.dividerColor;
    if (dc.size() == 7 && dc[0] == '#') {
        auto h = [&](int i) -> float {
            return std::stoul(dc.substr(i, 2), nullptr, 16) / 255.0f;
        };
        dividerR_ = h(1); dividerG_ = h(3); dividerB_ = h(5); dividerA_ = 1.0f;
    }

    auto parseTint = [](const std::string& col, float alpha, float out[4]) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (col.size() == 7 && col[0] == '#') {
            r = std::stoul(col.substr(1, 2), nullptr, 16) / 255.0f;
            g = std::stoul(col.substr(3, 2), nullptr, 16) / 255.0f;
            b = std::stoul(col.substr(5, 2), nullptr, 16) / 255.0f;
        }
        out[0] = r * alpha + (1.0f - alpha);
        out[1] = g * alpha + (1.0f - alpha);
        out[2] = b * alpha + (1.0f - alpha);
        out[3] = 1.0f;
    };
    parseTint(options.activePaneTint,   options.activePaneTintAlpha,   activeTint_);
    parseTint(options.inactivePaneTint, options.inactivePaneTintAlpha, inactiveTint_);
    if (!options.replacementChar.empty())
        replacementChar_ = options.replacementChar;

    dividersDirty_ = true;
    tabBarConfig_ = options.tabBar;
    initTabBar(options.tabBar);
}

void PlatformDawn::invalidateAllRowCaches()
{
    // Signal the render thread to invalidate all row caches on the next frame.
    // The render-private maps are only touched by the render thread, so we
    // communicate via the pending mutations flag.
    renderThread_->pending().invalidateAllRowCaches = true;
    for (Tab tab : tabs()) {
        if (tab.valid()) {
            for (Terminal* panePtr : tab.panes())
                renderThread_->pending().dirtyPanes.insert(panePtr->id());
        }
    }
}

static void applyTintColor(const std::string& col, float alpha, float out[4])
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (col.size() == 7 && col[0] == '#') {
        r = std::stoul(col.substr(1, 2), nullptr, 16) / 255.0f;
        g = std::stoul(col.substr(3, 2), nullptr, 16) / 255.0f;
        b = std::stoul(col.substr(5, 2), nullptr, 16) / 255.0f;
    }
    out[0] = r * alpha + (1.0f - alpha);
    out[1] = g * alpha + (1.0f - alpha);
    out[2] = b * alpha + (1.0f - alpha);
    out[3] = 1.0f;
}

void PlatformDawn::applyFontChange(const Config& config)
{
    std::string fontPath = config.font.empty() ? resolveFontFamily("monospace") : resolveFontFamily(config.font);
    if (fontPath.empty() && !config.font.empty())
        fontPath = resolveFontFamily("monospace");
    if (fontPath.empty()) {
        spdlog::warn("Config reload: font '{}' not found, keeping current font", config.font);
        return;
    }

    auto fontData = loadFontFile(fontPath);
    if (fontData.empty()) {
        spdlog::warn("Config reload: failed to load font '{}', keeping current font", fontPath);
        return;
    }

    std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};
    bool hasBoldFont = false;
    if (!config.font.empty()) {
        std::string boldPath = resolveFontFamily(config.font, FontTraitBold);
        if (!boldPath.empty() && boldPath != fontPath) {
            auto boldData = loadFontFile(boldPath);
            if (!boldData.empty()) {
                fontList.push_back(std::move(boldData));
                hasBoldFont = true;
            }
        }
    }

    // Tear down old font (GPU removal deferred to render thread)
    textSystem_.unregisterFont(fontName_);
    renderThread_->pending().mainFontRemoved = true;

    // Re-register
    textSystem_.registerFont(fontName_, fontList, 48.0f);
    primaryFontPath_ = fontPath;
    textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
    textSystem_.setSystemFallback([this](const std::string& path, char32_t cp) {
        return fontFallback_.fontDataForCodepoint(path, cp);
    });
    textSystem_.setEmojiFallback([this](char32_t cp) {
        return fontFallback_.fontDataForEmoji(cp);
    });

    if (!hasBoldFont)
        textSystem_.addSyntheticBoldVariant(fontName_, config.bold_strength, config.bold_strength);
    textSystem_.setBoldStrength(config.bold_strength, config.bold_strength);

    auto nerdFontPath = fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf";
    auto nerdData = loadFontFile(nerdFontPath.string());
    if (!nerdData.empty())
        textSystem_.addFallbackFont(fontName_, nerdData);

    terminalOptions().font = config.font;

    // Recalculate metrics
    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) { spdlog::error("Config reload: font re-registration failed"); return; }

    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

    renderThread_->pending().mainFontAtlasChanged = true;
    invalidateAllRowCaches();
    applyFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));

    spdlog::info("Config reload: font changed to '{}'", fontPath);
}

void PlatformDawn::onBlinkTick()
{
    auto tab = activeTab();
    if (!tab) return;
    auto wantsRedraw = [](TerminalEmulator* term) {
        return term && term->cursorBlinking() && term->cursorVisible();
    };
    if (!tab->valid()) return;
    for (Terminal* panePtr : tab->panes()) {
        if (wantsRedraw(panePtr)) {
            setNeedsRedraw();
            return;
        }
        for (const auto& popup : panePtr->popups()) {
            if (wantsRedraw(popup.get())) {
                setNeedsRedraw();
                return;
            }
        }
    }
}

void PlatformDawn::onResizeDebounceFire(uint32_t w, uint32_t h)
{
    // Lightweight update during live drag: signal the render thread
    // to reconfigure the surface and release stale textures.
    // The heavy work (terminal reflow, GPU buffer updates, dividers)
    // is deferred to onLiveResizeEnd → flushPendingFramebufferResize.
    {
        std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
        fbWidth_  = w;
        fbHeight_ = h;
        renderThread_->pending().surfaceNeedsReconfigure = true;
        renderThread_->pending().viewportSizeChanged = true;
        renderThread_->pending().releaseAllPaneTextures = true;
        renderThread_->pending().releaseTabBarTexture = true;
        tabBarDirty_ = true;
    }
    setNeedsRedraw();
    wakeRenderThread();
}

void PlatformDawn::applyConfig(const Config& config)
{
    spdlog::info("Config: hot-reload triggered");

    // Hold renderThread_->mutex() for the duration of the reload so that font
    // registration/unregistration in textSystem_ can't overlap with the
    // render thread's shapeRun/shapeText/getFont calls.
    // Internal helpers (applyFramebufferResize, refreshDividers, etc.)
    // expect the lock to be held already.
    std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());

    // Keybindings
    std::vector<Binding> allKey = defaultBindings();
    auto userBindings = parseBindings(config.keybindings);
    allKey.insert(allKey.end(), userBindings.begin(), userBindings.end());
    std::vector<MouseBinding> allMouse = defaultMouseBindings();
    auto userMouseBindings = parseMouseBindings(config.mousebindings);
    allMouse.insert(allMouse.end(), userMouseBindings.begin(), userMouseBindings.end());
    if (inputController_) {
        inputController_->setKeyBindings(std::move(allKey));
        inputController_->setMouseBindings(std::move(allMouse));
        inputController_->resetSequenceMatcher();
        inputController_->setAltSendsEsc(config.alt_sends_esc);
        inputController_->setKeySequenceTimeoutMs(config.key_sequence_timeout_ms);
    }
    if (window_) window_->setAltSendsEsc(config.alt_sends_esc);

    // Colors
    TerminalOptions& opts = terminalOptions();
    opts.colors = config.colors;
    defaultFgColor_ = color::parseHexRGBA(config.colors.foreground, 0xFFDDDDDD);
    defaultBgColor_ = color::parseHexRGBA(config.colors.background, 0x00000000);
    notifyAllTerminals([&config](TerminalEmulator* term) {
        term->applyColorScheme(config.colors);
    });
    invalidateAllRowCaches();

    // Cursor
    opts.cursor = config.cursor;
    notifyAllTerminals([&config](TerminalEmulator* term) {
        term->applyCursorConfig(config.cursor);
    });
    if (animScheduler_) animScheduler_->applyBlinkConfig(config.cursor.blink_rate, config.cursor.blink_fps);

    // Divider
    int oldDividerWidth = dividerWidth_;
    dividerWidth_ = std::max(0, config.divider_width);
    const std::string& dc = config.divider_color;
    if (dc.size() == 7 && dc[0] == '#') {
        auto h = [&](int i) -> float { return std::stoul(dc.substr(i, 2), nullptr, 16) / 255.0f; };
        dividerR_ = h(1); dividerG_ = h(3); dividerB_ = h(5); dividerA_ = 1.0f;
    }

    // OSC 133 outline color — parse "#rrggbb" into packed ABGR u32 matching
    // the compute shader's unpack (byte 0 = R, byte 3 = A).
    {
        const std::string& oc = config.command_outline_color;
        if (oc.size() == 7 && oc[0] == '#') {
            auto b = [&](int i) -> uint32_t {
                return static_cast<uint32_t>(std::stoul(oc.substr(i, 2), nullptr, 16));
            };
            commandOutlineColor_ = b(1) | (b(3) << 8) | (b(5) << 16) | (0xFFu << 24);
        }
    }
    commandDimFactor_ = std::clamp(config.command_dim_factor, 0.0f, 1.0f);
    commandNavigationWrap_ = config.command_navigation_wrap;
    for (Tab t : tabs()) {
        if (t.valid()) t.setDividerPixels(dividerWidth_);
    }
    opts.dividerWidth = config.divider_width;
    opts.dividerColor = config.divider_color;
    bool dividerChanged = (dividerWidth_ != oldDividerWidth);

    // Tints
    applyTintColor(config.active_pane_tint,   config.active_pane_tint_alpha,   activeTint_);
    applyTintColor(config.inactive_pane_tint, config.inactive_pane_tint_alpha, inactiveTint_);
    opts.activePaneTint       = config.active_pane_tint;
    opts.activePaneTintAlpha  = config.active_pane_tint_alpha;
    opts.inactivePaneTint     = config.inactive_pane_tint;
    opts.inactivePaneTintAlpha= config.inactive_pane_tint_alpha;

    // Padding
    float newPL = config.padding.left   * contentScaleX_;
    float newPT = config.padding.top    * contentScaleX_;
    float newPR = config.padding.right  * contentScaleX_;
    float newPB = config.padding.bottom * contentScaleX_;
    bool paddingChanged = (newPL != padLeft_ || newPT != padTop_ ||
                           newPR != padRight_ || newPB != padBottom_);
    padLeft_ = newPL; padTop_ = newPT; padRight_ = newPR; padBottom_ = newPB;
    opts.padding = config.padding;

    // Replacement char
    if (!config.replacement_char.empty()) {
        replacementChar_ = config.replacement_char;
        opts.replacementChar = config.replacement_char;
    }

    // Bold strength (only if font isn't changing — applyFontChange handles it otherwise)
    bool fontNameChanged = (config.font != opts.font);
    if (!fontNameChanged) {
        textSystem_.setBoldStrength(config.bold_strength, config.bold_strength);
        opts.boldStrength = config.bold_strength;
    }

    // Tab bar
    tabBarConfig_ = config.tab_bar;
    textSystem_.unregisterFont(tabBarFontName_);
    renderThread_->pending().tabBarFontRemoved = true;
    initTabBar(config.tab_bar);

    // Font name or size change
    float newFontSize = config.font_size * contentScaleX_;
    if (fontNameChanged) {
        fontSize_ = newFontSize;
        baseFontSize_ = fontSize_;
        opts.fontSize = config.font_size;
        applyFontChange(config);
    } else if (newFontSize != fontSize_) {
        fontSize_ = newFontSize;
        baseFontSize_ = fontSize_;
        opts.fontSize = config.font_size;
        const FontData* font = textSystem_.getFont(fontName_);
        if (font) {
            float scale = fontSize_ / font->baseSize;
            lineHeight_ = font->lineHeight * scale;
            const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
            charWidth_ = shaped.width;
            if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;
        }
        invalidateAllRowCaches();
        applyFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
    } else if (paddingChanged || dividerChanged) {
        applyFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
    } else {
        // Color-only change: no layout recompute needed, just refresh divider geometry.
        if (auto active = activeTab()) refreshDividers(*active);
    }

    tabBarDirty_ = true;
    setNeedsRedraw();
    spdlog::info("Config reloaded: {} user keybindings", userBindings.size());
}

// ========================================================================
// Framebuffer resize and font scaling
// ========================================================================

void PlatformDawn::adjustFontSize(float delta)
{
    float newSize;
    if (delta == 0.0f) {
        newSize = baseFontSize_; // reset
    } else {
        newSize = fontSize_ + delta * contentScaleX_;
    }
    if (newSize < 6.0f * contentScaleX_ || newSize > 72.0f * contentScaleX_) return;
    fontSize_ = newSize;

    // Recalculate metrics
    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) return;
    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

    // Trigger resize of all panes (recalculates grid dimensions)
    onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
}


void PlatformDawn::onFramebufferResize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());

    // During a live window drag, debounce the full resize work (surface
    // reconfigure + layout recompute + pane/terminal reflow) to 25 ms.
    // One-off resizes (first show, programmatic, live-resize end) apply
    // immediately.
    const bool live = window_ && window_->inLiveResize();
    if (live) {
        if (animScheduler_) {
            animScheduler_->scheduleResize(static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
        }
        // Even while debounced, update fbWidth_/fbHeight_ so other main-thread
        // readers (tab bar layout, hit-testing) see the latest window size.
        // The actual surface reconfigure + reflow happens when the timer fires.
        fbWidth_  = static_cast<uint32_t>(width);
        fbHeight_ = static_cast<uint32_t>(height);
        setNeedsRedraw();
        return;
    }

    applyFramebufferResize(width, height);
}

void PlatformDawn::flushPendingFramebufferResize()
{
    // Called on main thread under renderThread_->mutex() from onLiveResizeEnd.
    if (!animScheduler_) return;
    uint32_t w = 0, h = 0;
    if (!animScheduler_->takePendingResize(w, h)) {
        // On XCB (and similar backends) the 25ms debounce timer fires before
        // the 100ms live-resize-end timer, consuming pendingResizeW_/H_ via
        // onResizeDebounceFire (lightweight surface reconfigure only).  When
        // onLiveResizeEnd fires, takePendingResize finds nothing — but the
        // full terminal reflow (and therefore SIGWINCH) was never done.
        // Fall back to the current fb dimensions which onResizeDebounceFire
        // kept up to date.
        w = fbWidth_;
        h = fbHeight_;
    }
    if (w && h)
        applyFramebufferResize(static_cast<int>(w), static_cast<int>(h));
}

void PlatformDawn::applyFramebufferResize(int width, int height)
{
    // Caller must hold renderThread_->mutex().
    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    renderThread_->pending().surfaceNeedsReconfigure = true;
    renderThread_->pending().viewportSizeChanged = true;
    dividersDirty_ = true;

    // Clear divider buffers for all tabs — geometry is now stale
    auto allTabs = tabs();
    for (Tab& t : allTabs) clearDividers(t);

    // Fb change isn't a tree mutation but still invalidates all rects. Mark
    // dirty so runLayoutIfDirty (invoked from buildRenderFrameState next
    // frame) does the full resize cascade. Also run it inline here so the
    // render thread sees fresh rects immediately, matching the pre-gate
    // synchronous behaviour.
    scriptEngine_.layoutTree().markDirty();
    runLayoutIfDirty();

    // Release all held textures — they're now the wrong size.
    renderThread_->pending().releaseAllPaneTextures = true;
    renderThread_->pending().releaseTabBarTexture = true;
    tabBarDirty_ = true;
    if (auto active = activeTab()) refreshDividers(*active);
    setNeedsRedraw();
}

// ========================================================================
// Tab bar
// ========================================================================

namespace {
uint32_t parseTabBarHexColor(const std::string& hex, uint32_t def = 0xFF000000) {
    return color::parseHexRGBA(hex, def);
}
}

void PlatformDawn::initTabBar(const TabBarConfig& cfg)
{
    auto setBarSlot = [this](int cells) {
        Uuid bar = scriptEngine_.primaryTabBarNode();
        if (bar.isNil()) return;
        LayoutTree& tree = scriptEngine_.layoutTree();
        if (const Node* n = tree.node(bar); n && !n->parent.isNil())
            tree.setSlotFixedCells(n->parent, bar, cells);
    };
    if (cfg.style == "hidden") {
        setBarSlot(0);
        return;
    }

    // For "auto" mode with 1 tab, still load fonts but set height to 0

    // Resolve font
    std::string fontPath = cfg.font.empty() ? primaryFontPath_
                                             : resolveFontFamily(cfg.font);
    if (fontPath.empty()) fontPath = primaryFontPath_;
    float fontSize = (cfg.font_size > 0.0f) ? cfg.font_size * contentScaleX_ : fontSize_;
    tabBarFontSize_ = fontSize;

    auto fontData = loadFontFile(fontPath);
    if (!fontData.empty()) {
        std::vector<std::vector<uint8_t>> fl = {fontData};
        auto nerdPath = (fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf").string();
        auto nerdData = loadFontFile(nerdPath);
        if (!nerdData.empty()) fl.push_back(std::move(nerdData));
        textSystem_.registerFont(tabBarFontName_, fl, 48.0f);
        textSystem_.setPrimaryFontPath(tabBarFontName_, fontPath);
    }

    const FontData* font = textSystem_.getFont(tabBarFontName_);
    if (font) {
        // GPU upload deferred to render thread via renderThread_->pending().tabBarFontAtlasChanged
        renderThread_->pending().tabBarFontAtlasChanged = true;
        float scale = tabBarFontSize_ / font->baseSize;
        tabBarLineHeight_ = font->lineHeight * scale;
        const auto& shaped = textSystem_.shapeText(tabBarFontName_, "M", tabBarFontSize_);
        tabBarCharWidth_ = shaped.width;
        if (tabBarCharWidth_ < 1.0f) tabBarCharWidth_ = tabBarFontSize_ * 0.6f;
    } else {
        tabBarLineHeight_ = lineHeight_;
        tabBarCharWidth_  = charWidth_;
    }

    // TabBar slot: 1 cell high when visible, 0 when hidden. One line of the
    // tab bar font is close to one terminal cell (same line-height math);
    // finer control can be added later via a dedicated JS binding.
    setBarSlot(tabBarVisible() ? 1 : 0);

    // Parse colors
    tbBgColor_         = parseTabBarHexColor(cfg.colors.background);
    tbActiveBgColor_   = parseTabBarHexColor(cfg.colors.active_bg);
    tbActiveFgColor_   = parseTabBarHexColor(cfg.colors.active_fg);
    tbInactiveBgColor_ = parseTabBarHexColor(cfg.colors.inactive_bg);
    tbInactiveFgColor_ = parseTabBarHexColor(cfg.colors.inactive_fg);

    // Parse progress bar settings
    progressBarHeight_ = cfg.progress_height * contentScaleX_;
    {
        uint8_t r, g, b;
        if (color::parseHex(cfg.progress_color, r, g, b)) {
            progressColorR_ = r / 255.0f;
            progressColorG_ = g / 255.0f;
            progressColorB_ = b / 255.0f;
        }
    }

    tabBarDirty_ = true;
}

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, uint32_t flags)
{
    return std::make_unique<PlatformDawn>(argc, argv, flags);
}
