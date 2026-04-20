#pragma once

#include "Terminal.h"
#include <memory>
#include <string>
#include <vector>

struct LayoutNode {
    enum class Dir { Horizontal, Vertical };

    bool isLeaf = true;
    int paneId = -1;        // leaf only

    Dir dir = Dir::Horizontal; // split only
    float ratio = 0.5f;        // split only
    std::unique_ptr<LayoutNode> first;  // split only
    std::unique_ptr<LayoutNode> second; // split only

    PaneRect rect; // computed by Layout::computeRects
};

class Layout {
public:
    Layout();

    // Allocate a fresh pane ID and create the root tree node for it.
    // Must be called once before use. No Terminal is inserted yet — call
    // insertTerminal() afterward to populate the slot. Returns the pane ID.
    int createPane();

    // Split the pane containing paneId along dir; returns the new pane's id.
    // The new slot has no Terminal yet — call insertTerminal() to populate it.
    // newIsFirst=true places the new pane as the first (left/top) child.
    int splitPane(int paneId, LayoutNode::Dir dir, float ratio = 0.5f, bool newIsFirst = false);

    // Insert a Terminal into a slot created by splitPane(). Sets the pane ID
    // on the Terminal. Returns the Terminal pointer for convenience.
    Terminal* insertTerminal(int paneId, std::unique_ptr<Terminal> t);

    // Remove a pane; its sibling collapses up to fill the parent split.
    // Returns the extracted Terminal so callers can defer its destruction
    // (Terminal lifetime must extend past the render thread's current
    // frame). Returns nullptr if the pane wasn't found or it would leave
    // the tab empty.
    std::unique_ptr<Terminal> extractPane(int paneId);

    Terminal* pane(int paneId);
    const std::vector<std::unique_ptr<Terminal>>& panes() const { return mPanes; }

    // Return the pixel rect for the layout node with the given pane ID.
    // Useful for reading geometry before a Terminal has been inserted.
    PaneRect nodeRect(int paneId) const;

    // Focus
    int focusedPaneId() const { return mFocusedPaneId; }
    void setFocusedPane(int id);
    Terminal* focusedPane();

    // Zoom: the zoomed pane gets the full window rect; others get zero area.
    // Calling zoomPane with the already-zoomed id toggles zoom off.
    bool isZoomed() const { return mZoomedPaneId >= 0; }
    int zoomedPaneId() const { return mZoomedPaneId; }
    void zoomPane(int id);
    void unzoom();

    // Divider between panes
    void setDividerPixels(int px) { dividerPixels_ = std::max(0, px); }
    int dividerPixels() const { return dividerPixels_; }

    // Tab bar height reservation
    void setTabBar(int height, const std::string& position) {
        tabBarHeight_ = height;
        tabBarPosition_ = position;
    }
    PaneRect tabBarRect(uint32_t windowW, uint32_t windowH) const;

    // Recompute all pane rects from window pixel dimensions
    void computeRects(uint32_t windowW, uint32_t windowH);

    // Return pane id at pixel (px, py), or -1
    int paneAtPixel(int px, int py) const;

    // Return pixel rects of all split dividers (one per split node).
    // dividerPixels: width/height of each divider in pixels.
    std::vector<PaneRect> dividerRects(int dividerPixels) const;

    // Move the pane's boundary on `axis` by `pixelDelta` pixels.
    // Prefers the trailing boundary (right for Horizontal, bottom for Vertical);
    // falls back to the leading boundary when the pane has no trailing split
    // on that axis (e.g. rightmost / bottommost pane). pixelDelta is signed:
    // positive = rightward (Horizontal) or downward (Vertical).
    // Returns false if no applicable split was found on that axis.
    bool resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta);

    const LayoutNode* root() const { return mRoot.get(); }

private:
    void computeRectsRecursive(LayoutNode* node, PaneRect rect);
    LayoutNode* findLeafForPane(int paneId, LayoutNode* node);
    // Returns true and removes the leaf, replacing parent split with sibling
    bool removeLeafRecursive(std::unique_ptr<LayoutNode>& node, int paneId);

    std::unique_ptr<LayoutNode> mRoot;
    std::vector<std::unique_ptr<Terminal>> mPanes;
    int mFocusedPaneId { -1 };
    int mZoomedPaneId { -1 };
    int tabBarHeight_ { 0 };
    std::string tabBarPosition_ { "bottom" };
    int dividerPixels_ { 1 };
};
