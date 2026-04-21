#pragma once

#include "LayoutTree.h"
#include "Terminal.h"
#include "Uuid.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Carried forward only for the public Dir enum; the old binary-split tree
// type was deleted when Layout was rebased on LayoutTree. Callers that used
// `layout->root()` to walk the tree have been migrated to tree-walking
// helpers on Layout itself (dividersWithOwnerPanes, etc.).
struct LayoutNode {
    enum class Dir { Horizontal, Vertical };
};

class Layout {
public:
    // Default ctor: Layout owns its own LayoutTree. Used by test fixtures
    // where there is no Engine.
    Layout();

    // Production ctor: Layout's subtree lives inside the provided shared tree
    // (Engine's, reachable from both JS bindings and platform code). `shared`
    // must outlive this Layout. Passing nullptr falls back to the owned-tree
    // behavior so callers don't have to special-case missing engines.
    explicit Layout(LayoutTree* shared);

    ~Layout();

    // Allocate a fresh pane ID. On first call (empty Layout), also creates the
    // initial Terminal node in the tree and focuses it. On subsequent calls
    // this is a pure ID counter — splitPane is what inserts new leaves.
    int createPane();

    // Split the pane containing paneId along dir; returns the new pane's id.
    // The new slot has no Terminal yet — call insertTerminal() to populate it.
    // newIsFirst=true places the new pane as the first (left/top) child.
    int splitPane(int paneId, LayoutNode::Dir dir, float ratio = 0.5f, bool newIsFirst = false);

    // Attach a Terminal to a slot created by createPane() / splitPane().
    // Sets the pane ID on the Terminal. Returns the Terminal pointer.
    Terminal* insertTerminal(int paneId, std::unique_ptr<Terminal> t);

    // Remove a pane. Its sibling (under a binary-split parent) collapses up
    // to fill the parent's slot. Returns the extracted Terminal so callers
    // can defer destruction past the current render frame. Returns nullptr
    // if the pane wasn't found or removing it would leave the tab empty.
    std::unique_ptr<Terminal> extractPane(int paneId);

    Terminal* pane(int paneId);
    const std::vector<std::unique_ptr<Terminal>>& panes() const { return mPanes; }

    // Return the pixel rect for the pane's tree slot. Useful for reading
    // geometry before a Terminal has been inserted (rects are populated by
    // computeRects).
    PaneRect nodeRect(int paneId) const;

    // Focus
    int focusedPaneId() const { return mFocusedPaneId; }
    void setFocusedPane(int id);
    Terminal* focusedPane();

    // Zoom: the zoomed pane gets the full content rect; others get zero area.
    bool isZoomed() const { return mZoomedPaneId >= 0; }
    int zoomedPaneId() const { return mZoomedPaneId; }
    void zoomPane(int id);
    void unzoom();

    void setDividerPixels(int px) { dividerPixels_ = std::max(0, px); }
    int dividerPixels() const { return dividerPixels_; }

    void setTabBar(int height, const std::string& position) {
        tabBarHeight_ = height;
        tabBarPosition_ = position;
    }
    PaneRect tabBarRect(uint32_t windowW, uint32_t windowH) const;

    // Recompute all pane rects from window pixel dimensions. After the call,
    // each Terminal's own rect() reflects its assigned pixel rect (via
    // Terminal::setRect), so existing renderer / hit-test paths that read
    // pane->rect() keep working unchanged.
    void computeRects(uint32_t windowW, uint32_t windowH);

    // Pane id at pixel (px, py), or -1. Reads Terminal::rect() directly
    // since computeRects populates it.
    int paneAtPixel(int px, int py) const;

    // Pixel rects of all split dividers across the subtree, one per
    // inter-child boundary in every Container.
    std::vector<PaneRect> dividerRects(int dividerPixels) const;

    // Same as dividerRects(), paired with the paneId of the leftmost/topmost
    // leaf beneath the divider's "first" (left/top) side. Matches the old
    // collectFirstPaneDividers() semantics used by per-pane divider GPU VB
    // storage in TabManager::refreshDividers.
    std::vector<std::pair<int, PaneRect>> dividersWithOwnerPanes(int dividerPixels) const;

    // Move the pane's boundary on `axis` by `pixelDelta` pixels.
    // Positive = rightward (Horizontal) / downward (Vertical). Prefers the
    // trailing boundary in the nearest ancestor container matching `axis`;
    // falls back to the leading boundary when the pane is last in that axis.
    // Returns false if no applicable container was found.
    bool resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta);

    // UUID of this Layout's root Container in the shared tree. Non-owning
    // callers (TabManager, ScriptEngine) use this to hook the subtree under a
    // higher-level parent or reference it from JS by UUID string.
    Uuid subtreeRoot() const { return subtreeRoot_; }

private:
    int leftmostPaneIdInSubtree(Uuid root) const;
    void ratioToStretch(float ratio, bool newIsFirst, int& newStretch, int& oldStretch) const;
    SplitDir convertDir(LayoutNode::Dir d) const {
        return d == LayoutNode::Dir::Horizontal ? SplitDir::Horizontal : SplitDir::Vertical;
    }

    // Either ownedTree_ holds the tree (default-ctor path) and tree_ points
    // at it, or ownedTree_ is null and tree_ points at an external tree
    // (production path). All other methods go through `tree_` uniformly.
    std::unique_ptr<LayoutTree> ownedTree_;
    LayoutTree* tree_ = nullptr;
    Uuid subtreeRoot_;

    std::unordered_map<int, Uuid>           paneIdToUuid_;
    std::unordered_map<Uuid, int, UuidHash> uuidToPaneId_;

    std::vector<std::unique_ptr<Terminal>> mPanes;
    int mFocusedPaneId = -1;
    int mZoomedPaneId  = -1;

    int tabBarHeight_ = 0;
    std::string tabBarPosition_ = "bottom";
    int dividerPixels_ = 1;

    uint32_t lastFbW_ = 0;
    uint32_t lastFbH_ = 0;
};
