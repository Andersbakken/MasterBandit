#include "Layout.h"
#include <spdlog/spdlog.h>
#include <algorithm>

Layout::Layout() = default;

Pane* Layout::createPane()
{
    int id = mNextPaneId++;
    auto pane = std::make_unique<Pane>(id);
    Pane* ptr = pane.get();
    mPanes.push_back(std::move(pane));

    if (!mRoot) {
        // First pane becomes the root leaf
        mRoot = std::make_unique<LayoutNode>();
        mRoot->isLeaf = true;
        mRoot->paneId = id;
        mFocusedPaneId = id;
    }

    return ptr;
}

int Layout::splitPane(int paneId, LayoutNode::Dir dir, float ratio)
{
    LayoutNode* leaf = findLeafForPane(paneId, mRoot.get());
    if (!leaf) {
        spdlog::warn("Layout::splitPane: pane {} not found", paneId);
        return -1;
    }

    int newId = mNextPaneId++;
    auto newPane = std::make_unique<Pane>(newId);
    mPanes.push_back(std::move(newPane));

    // Replace the leaf with a split node
    // The existing pane becomes `first`, the new pane becomes `second`
    auto split = std::make_unique<LayoutNode>();
    split->isLeaf = false;
    split->dir = dir;
    split->ratio = ratio;

    auto existingLeaf = std::make_unique<LayoutNode>();
    existingLeaf->isLeaf = true;
    existingLeaf->paneId = paneId;

    auto newLeaf = std::make_unique<LayoutNode>();
    newLeaf->isLeaf = true;
    newLeaf->paneId = newId;

    split->first = std::move(existingLeaf);
    split->second = std::move(newLeaf);

    // Swap the leaf's content with the new split node in-place
    leaf->isLeaf = false;
    leaf->paneId = -1;
    leaf->dir = dir;
    leaf->ratio = ratio;
    leaf->first = std::move(split->first);
    leaf->second = std::move(split->second);

    return newId;
}

void Layout::removePane(int paneId)
{
    if (!mRoot) return;

    // Can't remove the last pane
    if (mRoot->isLeaf && mRoot->paneId == paneId) {
        spdlog::warn("Layout::removePane: cannot remove last pane");
        return;
    }

    removeLeafRecursive(mRoot, paneId);

    // Remove from panes list
    mPanes.erase(std::remove_if(mPanes.begin(), mPanes.end(),
                                [paneId](const std::unique_ptr<Pane>& p) {
                                    return p->id() == paneId;
                                }), mPanes.end());

    if (mFocusedPaneId == paneId) {
        mFocusedPaneId = mPanes.empty() ? -1 : mPanes.front()->id();
    }
    if (mZoomedPaneId == paneId) {
        mZoomedPaneId = -1;
    }
}

bool Layout::removeLeafRecursive(std::unique_ptr<LayoutNode>& node, int paneId)
{
    if (!node || node->isLeaf) return false;

    // Check if first child is the leaf to remove
    if (node->first && node->first->isLeaf && node->first->paneId == paneId) {
        node = std::move(node->second);
        return true;
    }
    // Check if second child is the leaf to remove
    if (node->second && node->second->isLeaf && node->second->paneId == paneId) {
        node = std::move(node->first);
        return true;
    }
    // Recurse
    return removeLeafRecursive(node->first, paneId) ||
           removeLeafRecursive(node->second, paneId);
}

Pane* Layout::pane(int paneId)
{
    for (auto& p : mPanes)
        if (p->id() == paneId) return p.get();
    return nullptr;
}

void Layout::setFocusedPane(int id)
{
    if (pane(id)) mFocusedPaneId = id;
}

Pane* Layout::focusedPane()
{
    return pane(mFocusedPaneId);
}

void Layout::zoomPane(int id)
{
    if (mZoomedPaneId == id) {
        unzoom();
    } else {
        mZoomedPaneId = id;
        spdlog::info("Layout: zoomed pane {}", id);
    }
}

void Layout::unzoom()
{
    mZoomedPaneId = -1;
    spdlog::info("Layout: unzoomed");
}

PaneRect Layout::tabBarRect(uint32_t w, uint32_t h) const
{
    if (tabBarHeight_ <= 0) return {};
    if (tabBarPosition_ == "top")
        return {0, 0, static_cast<int>(w), tabBarHeight_};
    return {0, static_cast<int>(h) - tabBarHeight_, static_cast<int>(w), tabBarHeight_};
}

void Layout::computeRects(uint32_t windowW, uint32_t windowH)
{
    if (!mRoot) return;

    // Reserve space for tab bar
    PaneRect full { 0, 0, static_cast<int>(windowW), static_cast<int>(windowH) };
    if (tabBarHeight_ > 0) {
        if (tabBarPosition_ == "top") {
            full.y = tabBarHeight_;
            full.h = static_cast<int>(windowH) - tabBarHeight_;
        } else {
            full.h = static_cast<int>(windowH) - tabBarHeight_;
        }
    }

    if (mZoomedPaneId >= 0) {
        // Give zoomed pane the full rect; all others get zero area
        for (auto& p : mPanes) {
            if (p->id() == mZoomedPaneId)
                p->setRect(full);
            else
                p->setRect({});
        }
    } else {
        computeRectsRecursive(mRoot.get(), full);
    }
}

void Layout::computeRectsRecursive(LayoutNode* node, PaneRect rect)
{
    if (!node) return;
    node->rect = rect;

    if (node->isLeaf) {
        if (Pane* p = pane(node->paneId))
            p->setRect(rect);
        return;
    }

    PaneRect firstRect = rect;
    PaneRect secondRect = rect;

    if (node->dir == LayoutNode::Dir::Horizontal) {
        firstRect.w  = static_cast<int>(rect.w * node->ratio);
        secondRect.x = rect.x + firstRect.w;
        secondRect.w = rect.w - firstRect.w;
    } else {
        firstRect.h  = static_cast<int>(rect.h * node->ratio);
        secondRect.y = rect.y + firstRect.h;
        secondRect.h = rect.h - firstRect.h;
    }

    computeRectsRecursive(node->first.get(), firstRect);
    computeRectsRecursive(node->second.get(), secondRect);
}

LayoutNode* Layout::findLeafForPane(int paneId, LayoutNode* node)
{
    if (!node) return nullptr;
    if (node->isLeaf) return node->paneId == paneId ? node : nullptr;
    if (auto* found = findLeafForPane(paneId, node->first.get())) return found;
    return findLeafForPane(paneId, node->second.get());
}

int Layout::paneAtPixel(int px, int py) const
{
    for (const auto& p : mPanes) {
        const PaneRect& r = p->rect();
        if (!r.isEmpty() && px >= r.x && px < r.x + r.w &&
                            py >= r.y && py < r.y + r.h)
            return p->id();
    }
    return -1;
}
