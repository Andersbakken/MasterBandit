#include "Layout.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <optional>

Layout::Layout() = default;

static int sGlobalPaneId = 0;

Pane* Layout::createPane()
{
    int id = sGlobalPaneId++;
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

int Layout::splitPane(int paneId, LayoutNode::Dir dir, float ratio, bool newIsFirst)
{
    LayoutNode* leaf = findLeafForPane(paneId, mRoot.get());
    if (!leaf) {
        spdlog::warn("Layout::splitPane: pane {} not found", paneId);
        return -1;
    }

    int newId = sGlobalPaneId++;
    auto newPane = std::make_unique<Pane>(newId);
    mPanes.push_back(std::move(newPane));

    auto existingLeaf = std::make_unique<LayoutNode>();
    existingLeaf->isLeaf = true;
    existingLeaf->paneId = paneId;

    auto newLeaf = std::make_unique<LayoutNode>();
    newLeaf->isLeaf = true;
    newLeaf->paneId = newId;

    // Replace the leaf in-place with a split node.
    leaf->isLeaf = false;
    leaf->paneId = -1;
    leaf->dir    = dir;
    leaf->ratio  = newIsFirst ? (1.0f - ratio) : ratio;
    if (newIsFirst) {
        leaf->first  = std::move(newLeaf);
        leaf->second = std::move(existingLeaf);
    } else {
        leaf->first  = std::move(existingLeaf);
        leaf->second = std::move(newLeaf);
    }

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
        secondRect.x = rect.x + firstRect.w + dividerPixels_;
        secondRect.w = rect.w - firstRect.w - dividerPixels_;
    } else {
        firstRect.h  = static_cast<int>(rect.h * node->ratio);
        secondRect.y = rect.y + firstRect.h + dividerPixels_;
        secondRect.h = rect.h - firstRect.h - dividerPixels_;
    }
    if (secondRect.w < 0) secondRect.w = 0;
    if (secondRect.h < 0) secondRect.h = 0;

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

static bool subtreeContains(int paneId, const LayoutNode* node)
{
    if (!node) return false;
    if (node->isLeaf) return node->paneId == paneId;
    return subtreeContains(paneId, node->first.get()) ||
           subtreeContains(paneId, node->second.get());
}

struct SplitFind { LayoutNode* node; bool inFirst; };

static std::optional<SplitFind> findSplitContaining(
    int paneId, LayoutNode::Dir dir, LayoutNode* node)
{
    if (!node || node->isLeaf) return std::nullopt;

    bool firstHas  = subtreeContains(paneId, node->first.get());
    bool secondHas = subtreeContains(paneId, node->second.get());

    if (node->dir == dir) {
        if (firstHas) {
            // Prefer a closer split inside first subtree
            auto inner = findSplitContaining(paneId, dir, node->first.get());
            return inner ? inner : SplitFind{node, true};
        }
        if (secondHas) {
            auto inner = findSplitContaining(paneId, dir, node->second.get());
            return inner ? inner : SplitFind{node, false};
        }
    } else {
        if (firstHas)  return findSplitContaining(paneId, dir, node->first.get());
        if (secondHas) return findSplitContaining(paneId, dir, node->second.get());
    }
    return std::nullopt;
}

bool Layout::growPane(int paneId, LayoutNode::Dir splitDir, int deltaPixels)
{
    auto result = findSplitContaining(paneId, splitDir, mRoot.get());
    if (!result) return false;

    LayoutNode* splitNode = result->node;
    float totalDim = (splitDir == LayoutNode::Dir::Horizontal)
                     ? static_cast<float>(splitNode->rect.w)
                     : static_cast<float>(splitNode->rect.h);
    if (totalDim <= 0.0f) return false;

    float deltaRatio = static_cast<float>(deltaPixels) / totalDim;
    // If pane is in first (left/top) child: growing it increases ratio.
    // If pane is in second (right/bottom) child: growing it decreases ratio.
    if (!result->inFirst) deltaRatio = -deltaRatio;

    splitNode->ratio = std::clamp(splitNode->ratio + deltaRatio, 0.05f, 0.95f);
    return true;
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

static void collectDividerRects(const LayoutNode* node, int divPx, std::vector<PaneRect>& out)
{
    if (!node || node->isLeaf || divPx <= 0) return;

    // The divider occupies the gap between first and second child rects.
    const PaneRect& r = node->rect;
    if (node->dir == LayoutNode::Dir::Horizontal) {
        int splitX = node->first ? (node->first->rect.x + node->first->rect.w) : 0;
        out.push_back({splitX, r.y, divPx, r.h});
    } else {
        int splitY = node->first ? (node->first->rect.y + node->first->rect.h) : 0;
        out.push_back({r.x, splitY, r.w, divPx});
    }

    collectDividerRects(node->first.get(),  divPx, out);
    collectDividerRects(node->second.get(), divPx, out);
}

std::vector<PaneRect> Layout::dividerRects(int dividerPixels) const
{
    std::vector<PaneRect> result;
    if (mRoot && mZoomedPaneId < 0)
        collectDividerRects(mRoot.get(), dividerPixels, result);
    return result;
}
