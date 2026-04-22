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
// per-tab state — icon, overlay stack, paneId index, focus, zoom, divider
// geometry — lives on Script::Engine (or on the tree node's `label`).
//
// Tab is a lightweight value type: default-constructible (invalid), copyable,
// movable. The underlying state is stable for as long as the engine still
// tracks the subtreeRoot; once closeTab removes it, stale Tab handles return
// null / empty. Pre-cutover callers that did `tab->layout()->foo()` still
// work — `layout()` returns `this` (self-pointer) and every Layout method
// now lives on Tab.
class Tab {
public:
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
    // Used by createTab's initial-pane bootstrap. Returns the fresh paneId.
    // Precondition: subtreeRoot is empty (no children).
    int createPane();

    // Allocate a Terminal tree node as an orphan (no parent) and associate
    // it with a fresh paneId in the engine's paneId↔Uuid index. The caller
    // is responsible for attaching it under some Container via appendChild
    // or splitByNodeId.
    int allocatePaneNode(Uuid* outNodeId = nullptr);

    // Wrap `existingChildNodeId` in a new Container, place `newChildNodeId`
    // alongside. Validates that both are known nodes, newChild is orphaned,
    // and existingChild has a parent. Returns false on any violation.
    bool splitByNodeId(Uuid existingChildNodeId, LayoutNode::Dir dir,
                       Uuid newChildNodeId, bool newIsFirst = false);

    // Attach a Terminal to a pane slot allocated via allocatePaneNode /
    // createPane. Threads id + nodeId onto the Terminal and transfers
    // ownership to the engine's terminals_ map.
    Terminal* insertTerminal(int paneId, std::unique_ptr<Terminal> t);

    // Remove an arbitrary subtree from this tab's subtree. Guard: any
    // descendant Terminal still live in the engine map → refuse. Cleans up
    // the paneId index on success. Returns false if guard trips or the
    // node is the subtreeRoot itself (use closeTab).
    bool removeNodeSubtree(Uuid nodeId);

    // --- Pane queries -----------------------------------------------------
    Terminal* pane(int paneId);
    bool hasPaneSlot(int paneId) const; // slot exists, Terminal may be dead
    std::vector<Terminal*> panes() const; // live Terminals in subtree (DFS)
    PaneRect nodeRect(int paneId) const;
    int paneAtPixel(int px, int py) const;

    // --- Focus (engine-wide) ----------------------------------------------
    int focusedPaneId() const;
    void setFocusedPane(int paneId);
    Terminal* focusedPane();

    // --- Zoom (engine-wide) -----------------------------------------------
    bool isZoomed() const;
    int zoomedPaneId() const;
    void zoomPane(int paneId);
    void unzoom();

    // --- Global layout params (forwarded to engine) -----------------------
    void setDividerPixels(int px);
    int dividerPixels() const;
    void setTabBar(int height, const std::string& position);
    PaneRect tabBarRect(uint32_t windowW, uint32_t windowH) const;

    // --- Rect computation + divider geometry ------------------------------
    void computeRects(uint32_t windowW, uint32_t windowH);
    std::vector<PaneRect> dividerRects(int dividerPixels) const;
    std::vector<std::pair<int, PaneRect>> dividersWithOwnerPanes(int dividerPixels) const;
    bool resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta);

private:
    Script::Engine* eng_ = nullptr;
    Uuid subtreeRoot_;
};
