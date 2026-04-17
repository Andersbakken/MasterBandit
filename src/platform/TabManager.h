#pragma once

#include "RenderSync.h"
#include "Tab.h"
#include "Terminal.h"
#include "TerminalOptions.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class EventLoop;
class Graveyard;
class InputController;
class Pane;
class RenderEngine;
class TerminalEmulator;
class Window;
namespace Script { class Engine; }
struct TerminalCallbacks;

// Owns the per-tab data and structural mutation flows: tab creation,
// closing, pane splitting side effects (spawn/resize), PTY poll
// registration, tab-title refresh, OS window title mirroring, and the
// terminalExited teardown path.
//
// PlatformDawn keeps tab-bar *rendering/config* state (config, font,
// colors, metrics); TabManager does not touch any of that — it just
// asks the host whether the bar is currently visible and pushes
// bar-dirty flags back through Host::markTabBarDirty().
//
// Host fields are split into two kinds:
//   - Immutable state set during construction (mutex, script engine,
//     input/render-engine pointers).
//   - std::function getters for state that is created lazily in
//     PlatformDawn::createTerminal (event loop, window) or varies
//     per-call (font metrics, fb size, padding).
class TabManager {
public:
    struct Host {
        // Coarse lock; held by the main thread around structural mutations
        // so the render thread can't snapshot mid-change.
        std::recursive_mutex* platformMutex = nullptr;

        // Script engine — notifications on tab/pane create/destroy,
        // resize, and input/output filter queries.
        Script::Engine* scriptEngine = nullptr;

        // Subsystems that outlive TabManager.
        InputController* inputController = nullptr;
        RenderEngine*    renderEngine    = nullptr;

        // Mutation accumulator owned by PlatformDawn; TabManager appends
        // structural ops and dirty flags but never consumes them.
        PendingMutations* pending = nullptr;

        // Lazy-created resources.
        std::function<EventLoop*()> eventLoop;
        std::function<Window*()>    window;

        // Tab-bar + window state.
        std::function<bool()> headless;
        std::function<bool()> tabBarVisible;
        std::function<void()> updateTabBarVisibility;
        std::function<void()> markTabBarDirty;
        std::function<void()> setNeedsRedraw;
        std::function<void()> quit;

        // Font / framebuffer / padding for pane + terminal resize.
        std::function<float()>    charWidth;
        std::function<float()>    lineHeight;
        std::function<float()>    padLeft;
        std::function<float()>    padTop;
        std::function<float()>    padRight;
        std::function<float()>    padBottom;
        std::function<uint32_t()> fbWidth;
        std::function<uint32_t()> fbHeight;

        // Tab bar metrics used by createTab when applying tab bar height.
        std::function<int()>         dividerWidth;
        std::function<float()>       tabBarLineHeight;
        std::function<std::string()> tabBarPosition;

        // Divider color for refreshDividers.
        std::function<float()> dividerR;
        std::function<float()> dividerG;
        std::function<float()> dividerB;
        std::function<float()> dividerA;

        // Queued main-thread lambda.
        std::function<void(Terminal*)> queueTerminalExit;

        // Build the terminal callbacks shim — stays on PlatformDawn
        // because it closes over ~20 platform members (title, icon,
        // CWD, progress, OSC, foreground-process, mouse-cursor-shape).
        std::function<TerminalCallbacks(int paneId)> buildTerminalCallbacks;

        // Main-thread graveyard for deferred destruction of Panes / Tabs /
        // overlays / PopupPanes. The Terminals they own must outlive the
        // render thread's current frame; the graveyard stamps each entry
        // with the current frame counter and sweeps once the counter has
        // advanced. platformMutex must be held when staging entries.
        Graveyard* graveyard = nullptr;

        // Read the render thread's completedFrames() counter. Used to
        // stamp graveyard entries. Should be read under platformMutex so
        // any frame in flight at the moment of staging has counter
        // <= stamp, and its completion will advance it past the stamp.
        std::function<uint64_t()> completedFrames;

        // Rebuild the render shadow copy from live state. Called under
        // platformMutex from structural mutation sites (tab close, pane
        // close) so the next render snapshot doesn't pick up dead
        // Terminal pointers between the mutation and applyPendingMutations().
        std::function<void()> buildRenderFrameState;
    };

    TabManager() = default;
    ~TabManager() = default;

    TabManager(const TabManager&) = delete;
    TabManager& operator=(const TabManager&) = delete;

    void setHost(Host host) { host_ = std::move(host); }

    // --- Accessors ---
    std::vector<std::unique_ptr<Tab>>& tabs() { return tabs_; }
    const std::vector<std::unique_ptr<Tab>>& tabs() const { return tabs_; }
    int activeTabIdx() const { return activeTabIdx_; }
    void setActiveTabIdx(int idx) { activeTabIdx_ = idx; }
    size_t size() const { return tabs_.size(); }

    Tab* activeTab() {
        if (tabs_.empty() || activeTabIdx_ < 0 ||
            activeTabIdx_ >= static_cast<int>(tabs_.size())) {
            return nullptr;
        }
        return tabs_[activeTabIdx_].get();
    }

    Tab* tabAt(int idx) {
        if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return nullptr;
        return tabs_[idx].get();
    }

    Terminal* activeTerm();

    void notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn);

    // --- Terminal options (shared spawn config) ---
    TerminalOptions& terminalOptions() { return terminalOptions_; }
    const TerminalOptions& terminalOptions() const { return terminalOptions_; }
    void setTerminalOptions(TerminalOptions opts) { terminalOptions_ = std::move(opts); }

    // --- PTY poll registry ---
    std::unordered_map<int, Terminal*>& ptyPolls() { return ptyPolls_; }
    void addPtyPoll(int fd, Terminal* term);
    void removePtyPoll(int fd);

    // --- Tab lifecycle ---
    // Add a pre-built tab and mark it as active. Used by the one-shot
    // createTerminal() path which builds the layout + terminal inline.
    void addInitialTab(std::unique_ptr<Tab> tab);

    void createTab();
    void closeTab(int idx);

    // Called from the onTerminalExited deferred drain in PlatformDawn.
    void terminalExited(Terminal* terminal);

    // --- Pane helpers ---
    void spawnTerminalForPane(Pane* pane, int tabIdx, const std::string& cwd = {});
    void resizeAllPanesInTab(Tab* tab);
    void refreshDividers(Tab* tab);
    void clearDividers(Tab* tab);
    void releaseTabTextures(Tab* tab);

    // --- Title flow ---
    void updateTabTitleFromFocusedPane(int tabIdx);
    void updateWindowTitle();
    void notifyPaneFocusChange(Tab* tab, int prevId, int newId);

    // --- Lookup helpers ---
    // Find the tab that contains a given pane; returns the tab pointer
    // and (optionally) its index. Returns nullptr if not found.
    Tab* findTabForPane(int paneId, int* outTabIdx = nullptr);

private:
    static std::string popupStateKey(int paneId, const std::string& popupId) {
        return std::to_string(paneId) + "/" + popupId;
    }

    Host host_;

    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeTabIdx_ = 0;
    std::unordered_map<int, Terminal*> ptyPolls_;
    TerminalOptions terminalOptions_;
};
