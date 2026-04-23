#pragma once

#include "LayoutTree.h"
#include "Uuid.h"

#include <memory>
#include <string>
#include <vector>

class Terminal;
namespace Script { class Engine; }

struct PaneRect;

// Split direction for Tab methods (also re-exported as LayoutNode::Dir via
// Layout.h for legacy action/binding code).
struct LayoutNode {
    enum class Dir { Horizontal, Vertical };
};

// Non-owning handle for a tab. A tab's identity is its subtreeRoot Uuid in
// the shared LayoutTree (a direct child of Engine::layoutRootStack_). All
// per-tab state — icon, overlay stack, focus, divider geometry — lives on
// Script::Engine (or on the tree node's `label`).
//
// Tab is a lightweight value type: default-constructible (invalid), copyable,
// movable. Two pointers only. The underlying state is stable for as long as
// the engine still tracks the subtreeRoot; once closeTab removes it, stale
// Tab handles return null / empty. `layout()` returns `this` as a self-
// pointer so legacy `tab->layout()->method()` call sites still compile
// without a second indirection.
struct Tab {
    Tab() = default;
    Tab(Script::Engine* eng, Uuid subtreeRoot) : eng_(eng), subtreeRoot_(subtreeRoot) {}

    // Create a fresh tab subtree on `engine`'s shared LayoutTree. The new
    // subtreeRoot is a Container and can be attached as a child of
    // engine->layoutRootStack() via TabManager::attachLayoutSubtree. Returns
    // an invalid handle when engine is null.
    static Tab newSubtree(Script::Engine* engine);

    bool valid() const { return eng_ && !subtreeRoot_.isNil(); }
    explicit operator bool() const { return valid(); }

    Uuid subtreeRoot() const { return subtreeRoot_; }
    Script::Engine* engine() const { return eng_; }

    // Backward-compat: legacy callers do `tab->layout()->method()`. We return
    // `this` so those patterns keep working until a cleanup pass sweeps them.
    Tab*       layout()       { return valid() ? this : nullptr; }
    const Tab* layout() const { return valid() ? this : nullptr; }

    // --- Title / icon -----------------------------------------------------
    // Title lives on the tree node's `label`. An invalid handle or missing
    // node returns an empty string reference. Icon lives in Engine::tabIcons_.
    const std::string& title() const;
    void setTitle(const std::string& s);
    const std::string& icon() const;
    void setIcon(const std::string& s);

    // --- Pane allocation / tree mutation ----------------------------------
    // Create a Container holding a single Terminal leaf under subtreeRoot.
    // Used by createTab's initial-pane bootstrap. Returns the Terminal's
    // tree Uuid. Precondition: subtreeRoot is empty (no children).
    Uuid createPane();

    // Allocate a Terminal tree node as an orphan (no parent). The caller
    // is responsible for attaching it under some Container via appendChild
    // or splitByNodeId. Returns the new node's Uuid.
    Uuid allocatePaneNode();

    // Wrap `existingChildNodeId` in a new Container, place `newChildNodeId`
    // alongside. Validates that both are known nodes, newChild is orphaned,
    // and existingChild has a parent. Returns false on any violation.
    bool splitByNodeId(Uuid existingChildNodeId, LayoutNode::Dir dir,
                       Uuid newChildNodeId, bool newIsFirst = false);

    // Attach a Terminal to a pane slot allocated via allocatePaneNode /
    // createPane. Threads nodeId onto the Terminal and transfers ownership
    // to the engine's terminals_ map.
    Terminal* insertTerminal(Uuid nodeId, std::unique_ptr<Terminal> t);

    // Remove an arbitrary subtree from this tab's subtree. Guard: any
    // descendant Terminal still live in the engine map → refuse. Cleans up
    // the paneId index on success. Returns false if guard trips or the
    // node is the subtreeRoot itself (use closeTab).
    bool removeNodeSubtree(Uuid nodeId);

    // --- Pane queries -----------------------------------------------------
    Terminal* pane(Uuid nodeId);
    bool hasPaneSlot(Uuid nodeId) const; // slot exists, Terminal may be dead
    // All live Terminals in the tab's subtree, DFS. Includes Terminals under
    // inactive Stack siblings (e.g. a non-visible content Container while a
    // pager overlay is active). Use for "every pane in this tab" work —
    // resize cascade, close-tab teardown, per-pane callbacks.
    std::vector<Terminal*> panes() const;
    // Live Terminals that are actually visible given the tree's activeChild
    // semantics — Stacks contribute only their activeChild's subtree, not
    // all children. Use this for rendering / dividers / hit-testing paths
    // where "what's on screen right now" matters.
    std::vector<Terminal*> activePanes() const;
    PaneRect nodeRect(Uuid nodeId) const;
    Uuid paneAtPixel(int px, int py) const;

    // --- Focus (engine-wide) ----------------------------------------------
    Uuid focusedPaneId() const;
    void setFocusedPane(Uuid nodeId);
    Terminal* focusedPane();

    // Zoom moved onto StackData::zoomTarget — JS (mb.layout.setStackZoom)
    // writes it directly; Tab no longer mediates.

    // --- Global layout params (forwarded to engine) -----------------------
    void setDividerPixels(int px);
    int dividerPixels() const;
    // Tab-bar geometry now lives on a TabBar node in the tree. `tabBarRect`
    // still exists as a convenience that runs a root-level computeRects and
    // returns the primary TabBar node's rect; use this from renderers/input
    // rather than carrying bar geometry on the engine.
    PaneRect tabBarRect(uint32_t windowW, uint32_t windowH) const;

    // --- Rect computation + divider geometry ------------------------------
    // cellW / cellH are pixels-per-cell along the container's split axis,
    // used to convert ChildSlot cell-based sizing hints (minCells, maxCells,
    // fixedCells) into pixels. Callers pass rounded font metrics; 1,1 is
    // legal but reduces cell-based clamps to pixel-based ones.
    void computeRects(uint32_t windowW, uint32_t windowH, int cellW, int cellH);
    std::vector<PaneRect> dividerRects(int dividerPixels) const;
    std::vector<std::pair<Uuid, PaneRect>> dividersWithOwnerPanes(int dividerPixels) const;
    bool resizePaneEdge(Uuid nodeId, LayoutNode::Dir axis, int pixelDelta);

private:
    Script::Engine* eng_ = nullptr;
    Uuid subtreeRoot_;
};
