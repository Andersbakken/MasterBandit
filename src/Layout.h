#pragma once

#include "Pane.h"
#include <memory>
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

    // Create the initial pane (must be called once before use)
    Pane* createPane();

    // Split the pane containing paneId along dir; returns the new pane's id
    int splitPane(int paneId, LayoutNode::Dir dir, float ratio = 0.5f);

    // Remove a pane; its sibling collapses up to fill the parent split
    void removePane(int paneId);

    Pane* pane(int paneId);
    const std::vector<std::unique_ptr<Pane>>& panes() const { return mPanes; }

    // Focus
    int focusedPaneId() const { return mFocusedPaneId; }
    void setFocusedPane(int id);
    Pane* focusedPane();

    // Zoom: the zoomed pane gets the full window rect; others get zero area.
    // Calling zoomPane with the already-zoomed id toggles zoom off.
    bool isZoomed() const { return mZoomedPaneId >= 0; }
    int zoomedPaneId() const { return mZoomedPaneId; }
    void zoomPane(int id);
    void unzoom();

    // Recompute all pane rects from window pixel dimensions
    void computeRects(uint32_t windowW, uint32_t windowH);

    // Return pane id at pixel (px, py), or -1
    int paneAtPixel(int px, int py) const;

    const LayoutNode* root() const { return mRoot.get(); }

private:
    void computeRectsRecursive(LayoutNode* node, PaneRect rect);
    LayoutNode* findLeafForPane(int paneId, LayoutNode* node);
    // Returns true and removes the leaf, replacing parent split with sibling
    bool removeLeafRecursive(std::unique_ptr<LayoutNode>& node, int paneId);

    std::unique_ptr<LayoutNode> mRoot;
    std::vector<std::unique_ptr<Pane>> mPanes;
    int mNextPaneId { 0 };
    int mFocusedPaneId { -1 };
    int mZoomedPaneId { -1 };
};
