#include "Layout.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <optional>

Layout::Layout() = default;

static int sGlobalPaneId = 0;

static int leftmostPaneId(const LayoutNode* node)
{
    if (!node) return -1;
    if (node->isLeaf) return node->paneId;
    return leftmostPaneId(node->first.get());
}

static int siblingPaneId(int paneId, const LayoutNode* node)
{
    if (!node || node->isLeaf) return -1;
    if (node->first && node->first->isLeaf && node->first->paneId == paneId)
        return leftmostPaneId(node->second.get());
    if (node->second && node->second->isLeaf && node->second->paneId == paneId)
        return leftmostPaneId(node->first.get());
    int r = siblingPaneId(paneId, node->first.get());
    if (r >= 0) return r;
    return siblingPaneId(paneId, node->second.get());
}

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

std::unique_ptr<Pane> Layout::extractPane(int paneId)
{
    if (!mRoot) return nullptr;

    // Can't remove the last pane
    if (mRoot->isLeaf && mRoot->paneId == paneId) {
        spdlog::warn("Layout::extractPane: cannot remove last pane");
        return nullptr;
    }

    // Find sibling before modifying the tree
    int newFocus = (mFocusedPaneId == paneId) ? siblingPaneId(paneId, mRoot.get()) : -1;

    removeLeafRecursive(mRoot, paneId);

    // Extract from panes list (move-out then erase, so the Pane survives
    // for the caller to hand to the Graveyard).
    std::unique_ptr<Pane> extracted;
    auto it = std::find_if(mPanes.begin(), mPanes.end(),
                           [paneId](const std::unique_ptr<Pane>& p) {
                               return p->id() == paneId;
                           });
    if (it != mPanes.end()) {
        extracted = std::move(*it);
        mPanes.erase(it);
    }

    if (mFocusedPaneId == paneId) {
        mFocusedPaneId = (newFocus >= 0) ? newFocus
                       : (mPanes.empty() ? -1 : mPanes.front()->id());
    }
    if (mZoomedPaneId == paneId) {
        mZoomedPaneId = -1;
    }
    return extracted;
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

// Find the deepest ancestor split on `axis` where paneId is reachable through
// the first (preferFirst=true) or second (preferFirst=false) subtree. The
// first-child case corresponds to the pane's trailing (right/bottom) boundary;
// the second-child case corresponds to its leading (left/top) boundary.
static LayoutNode* findBoundarySplit(int paneId, LayoutNode::Dir axis,
                                     LayoutNode* node, bool preferFirst)
{
    if (!node || node->isLeaf) return nullptr;

    bool firstHas  = subtreeContains(paneId, node->first.get());
    bool secondHas = subtreeContains(paneId, node->second.get());

    LayoutNode* child = firstHas ? node->first.get()
                       : (secondHas ? node->second.get() : nullptr);
    if (LayoutNode* deeper = findBoundarySplit(paneId, axis, child, preferFirst))
        return deeper;

    if (node->dir == axis) {
        if (preferFirst  && firstHas)  return node;
        if (!preferFirst && secondHas) return node;
    }
    return nullptr;
}

bool Layout::resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta)
{
    // Prefer the trailing boundary (pane is first child of the split).
    LayoutNode* split = findBoundarySplit(paneId, axis, mRoot.get(), /*preferFirst*/true);
    if (!split)
        split = findBoundarySplit(paneId, axis, mRoot.get(), /*preferFirst*/false);
    if (!split) return false;

    float totalDim = (axis == LayoutNode::Dir::Horizontal)
                     ? static_cast<float>(split->rect.w)
                     : static_cast<float>(split->rect.h);
    if (totalDim <= 0.0f) return false;

    float deltaRatio = static_cast<float>(pixelDelta) / totalDim;
    split->ratio = std::clamp(split->ratio + deltaRatio, 0.05f, 0.95f);
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
