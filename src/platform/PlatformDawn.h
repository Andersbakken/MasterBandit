#pragma once

#include "InputTypes.h"
#include "Terminal.h"
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

#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <uv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

class PlatformDawn {
public:
    PlatformDawn(int argc, char** argv, bool headless = false);
    ~PlatformDawn();

    int exec();
    void quit(int status = 0);
    void createTerminal(const TerminalOptions& options);
    void createTab();
    void closeTab(int idx);

    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }
    TexturePool& texturePool() { return texturePool_; }
    bool isHeadless() const { return headless_; }

    void setTestConfig(const std::string& fontPath, int cols, int rows, float fontSize) {
        testFontPath_ = fontPath;
        testCols_ = cols;
        testRows_ = rows;
        testFontSize_ = fontSize;
    }

    const std::string& exeDir() const { return exeDir_; }

    // Called from GLFW callbacks
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint);
    void onFramebufferResize(int width, int height);
    void adjustFontSize(float delta);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    void renderFrame();
    bool shouldClose() {
        if (headless_) return false;
        return glfwWindow_ && glfwWindowShouldClose(glfwWindow_);
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

    // Headless / test mode
    bool headless_ = false;
    bool platformInitialized_ = false;
    int testCols_ = 80;
    int testRows_ = 24;
    float testFontSize_ = 16.0f;
    std::string testFontPath_;
    wgpu::Texture headlessComposite_;  // offscreen composite target (headless, with CopyDst)
    uv_loop_t* loop_ = nullptr;
    uv_idle_t idleCb_ = {};
    std::string exeDir_;
    std::unique_ptr<DebugIPC> debugIPC_;
    std::shared_ptr<DebugIPCSink> debugSink_;

    // Tabs
    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeTabIdx_ = 0;
    int lastFocusedPaneId_ = -1;

    void updateTabBarVisibility() {
        if (tabBarConfig_.style != "auto") return;
        int h = tabBarVisible() ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
        for (auto& tab : tabs_) {
            tab->layout()->setTabBar(h, tabBarConfig_.position);
            tab->layout()->computeRects(fbWidth_, fbHeight_);
            for (auto& p : tab->layout()->panes())
                p->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);
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
                    if (auto* term = pane->activeTerm()) fn(term);
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
    GLFWwindow* glfwWindow_ = nullptr;
    wgpu::Surface surface_;
    Renderer renderer_;
    TextSystem textSystem_;
    std::string fontName_ = "mono";
    float fontSize_ = 16.0f;
    float baseFontSize_ = 0.0f;
    float charWidth_ = 0.0f;
    float lineHeight_ = 0.0f;
    float contentScaleX_ = 1.0f, contentScaleY_ = 1.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;
    std::string primaryFontPath_;
    FontFallback fontFallback_;

    // Default colors
    uint32_t defaultFgColor_ = 0xFFDDDDDD;
    uint32_t defaultBgColor_ = 0x00000000;
    bool needsRedraw_ = true;
    bool controlPressed_ = false;
    unsigned int lastMods_ = 0;

    // Per-pane render state
    struct PaneRenderState {
        std::vector<ResolvedCell> resolvedCells;
        std::vector<GlyphEntry> glyphBuffer;
        uint32_t totalGlyphs = 0;
        int lastCursorX = -1, lastCursorY = -1;
        bool lastCursorVisible = true;
        PooledTexture* heldTexture = nullptr;
        bool dirty = true;
        std::vector<PooledTexture*> pendingRelease;
        wgpu::Buffer dividerVB;
        int lastViewportOffset = 0;
        int lastHistorySize = 0;

        struct RowGlyphCache {
            std::vector<GlyphEntry> glyphs;
            std::vector<std::pair<uint32_t, uint32_t>> cellGlyphRanges;
            bool valid = false;
        };
        std::vector<RowGlyphCache> rowShapingCache;
    };
    std::unordered_map<int, PaneRenderState> paneRenderStates_;
    std::unordered_map<Tab*, PaneRenderState> overlayRenderStates_; // per-tab overlay render state

    void resolveRow(PaneRenderState& rs, TerminalEmulator* term, int row, FontData* font, float scale);

    std::unordered_map<int, uv_poll_t*> ptyPolls_;

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

    // Keybindings
    std::vector<Binding> bindings_;
    SequenceMatcher      sequenceMatcher_;
    void dispatchAction(const Action::Any& action);
    void terminalExited(Terminal* terminal);

    // Pane helpers
    void spawnTerminalForPane(Pane* pane, int tabIdx);
    void resizeAllPanesInTab(Tab* tab);
    void refreshDividers(Tab* tab);
    void clearDividers(Tab* tab);
    void releaseTabTextures(Tab* tab);
    void updateTabTitleFromFocusedPane(int tabIdx);
    void notifyPaneFocusChange(Tab* tab, int prevId, int newId);
};

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, bool headless = false);
