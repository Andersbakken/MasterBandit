#pragma once

#include "Layout.h"
#include "RenderSync.h"
#include "Tab.h"
#include "Terminal.h"
#include "TerminalOptions.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class EventLoop;
class Graveyard;
class InputController;
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
        std::function<TerminalCallbacks(Uuid nodeId)> buildTerminalCallbacks;

        // Main-thread graveyard for deferred destruction of Panes / Tabs /
        // overlays / Terminals. The Terminals they own must outlive the
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
    // Tab identity lives in the shared LayoutTree: each tab is a direct
    // child of Script::Engine::layoutRootStack_, identified by its
    // subtreeRoot Uuid. The vector returned by tabs() is built on demand
    // from the tree's root-Stack children, so iterators are valid only
    // until the next structural mutation. The Tab handle itself is a thin
    // value type — copy it around freely.
    std::vector<Tab> tabs() const;
    int activeTabIdx() const;
    void setActiveTabIdx(int idx);
    size_t size() const;

    // Returns nullopt when no tabs exist. `std::optional<Tab>` preserves
    // `tab->method()` syntax via optional::operator->.
    std::optional<Tab> activeTab() const;
    std::optional<Tab> tabAt(int idx) const;

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

    // Attach a tab's subtree (its subtreeRoot is an orphan Container in the
    // shared tree) as a direct child of the root Stack, optionally setting
    // it as the Stack's activeChild. No-op when there is no Script::Engine
    // (test paths).
    void attachLayoutSubtree(Tab tab, bool activate);

    // --- Tab lifecycle ---
    // Register a pre-built Tab (its subtreeRoot already holds Terminals) as
    // the initial tab, activated. Used by the one-shot createTerminal() path
    // which builds the subtree + first Terminal inline before TabManager is
    // fully wired up.
    void addInitialTab(Tab tab);

    void closeTab(int idx);

    // --- JS-facing primitives (mb.layout.*) -------------------------------
    // The default UI controller composes tab lifecycle out of these pieces
    // (createEmptyTab + createTerminalInContainer, splitPaneByNodeId, etc.)
    // so native doesn't need an all-in-one createTab path anymore.

    // Build an empty Tab (no initial Terminal) and attach its Layout subtree
    // under the shared tree's root Stack. Does NOT activate. Returns the new
    // tab index; optionally fills outNodeId with the Layout's subtreeRoot
    // UUID (the "tab node" surfaced to JS).
    int createEmptyTab(Uuid* outNodeId = nullptr);

    // Activate tab at `idx`, wrapping all the UI chrome updates (clear/refresh
    // dividers, update window title, tab-bar dirty, redraw). No-op if `idx`
    // is out of range.
    void activateTabByIdx(int idx);

    // Spawn a Terminal inside the Layout that owns `parentContainerNodeId`,
    // append the new Terminal node as the container's last child, and fire
    // `paneCreated`. On success returns true and fills `outNodeId` with the
    // fresh Terminal's tree Uuid. `cwd` may be empty.
    bool createTerminalInContainer(Uuid parentContainerNodeId,
                                   const std::string& cwd,
                                   Uuid* outNodeId);

    // Wrap `existingPaneNodeId` in a new Container, place a freshly-spawned
    // Terminal alongside, and fire `paneCreated`.
    bool splitPaneByNodeId(Uuid existingPaneNodeId, LayoutNode::Dir dir,
                           float ratio, bool newIsFirst,
                           Uuid* outNodeId);

    // Remove a node (Terminal leaf, Container, or Stack) from its enclosing
    // Tab's Layout tree. Refuses if any descendant Terminal is still live in
    // the engine map — kill them via killTerminal first. Also refuses if
    // `nodeId` is the Tab's subtreeRoot (use closeTab for that).
    //
    // Side effects: pane-index cleanup, focus/zoom fixup (handled inside
    // Layout::removeNodeSubtree), shadow-copy rebuild, resize + focus-chrome
    // refresh for the surviving tab content. Caller need not hold the
    // platform mutex; this method takes it internally.
    bool removeNode(Uuid nodeId);

    // Set the focused pane in whichever tab contains it, with notify
    // + title update + redraw. Returns false if pane not found.
    bool focusPaneById(Uuid nodeId);

    // Lookup helpers for the binding layer.
    std::optional<Tab> findTabBySubtreeRoot(Uuid subtreeRoot, int* outTabIdx = nullptr) const;
    // Walk up from `nodeId`'s ancestors until we hit a Layout's subtreeRoot;
    // returns the owning Tab (or nullopt).
    std::optional<Tab> findTabForNode(Uuid nodeId, int* outTabIdx = nullptr) const;

    // Called from the onTerminalExited deferred drain in PlatformDawn.
    // Resolves the Terminal's nodeId and delegates to killTerminal.
    void terminalExited(Terminal* terminal);

    // Synchronous Terminal kill: remove PTY poll, queue render-state cleanup,
    // extract from Script::Engine's map, graveyard with the current frame
    // stamp, and fire the `terminalExited` JS event. The Terminal's tree
    // node is left in place — the JS controller is responsible for removing
    // it (via removeNode) and deciding whether to close the
    // enclosing tab or quit. Caller must hold host_.platformMutex.
    // Returns true when a matching live Terminal was found and killed.
    bool killTerminal(Uuid nodeId);

    // --- Pane helpers ---
    void spawnTerminalForPane(Uuid nodeId, int tabIdx, const std::string& cwd = {});
    void resizeAllPanesInTab(Tab tab);
    void refreshDividers(Tab tab);
    void clearDividers(Tab tab);
    void releaseTabTextures(Tab tab);

    // --- Title flow ---
    void updateTabTitleFromFocusedPane(int tabIdx);
    void updateWindowTitle();
    void notifyPaneFocusChange(Tab tab, Uuid prevId, Uuid newId);

    // --- Lookup helpers ---
    // Find the tab that contains a given pane; returns a handle and
    // (optionally) its index. Returns nullopt if not found.
    std::optional<Tab> findTabForPane(Uuid nodeId, int* outTabIdx = nullptr) const;

private:
    static std::string popupStateKey(Uuid nodeId, const std::string& popupId) {
        return nodeId.toString() + "/" + popupId;
    }

    // Walk layoutRootStack_'s children and return the Nth child's UUID;
    // Uuid{} if out of range.
    Uuid tabSubtreeRootAt(int idx) const;

    Host host_;

    std::unordered_map<int, Terminal*> ptyPolls_;
    TerminalOptions terminalOptions_;
};
