#include "Layout.h"

#include "script/ScriptEngine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <functional>

// Global pane-id counter. Preserved as today so existing external code (JS
// bindings that wrap integer pane ids, per-pane GPU buffers keyed by id,
// TabManager pane lookups) sees the same stable-enough-for-session values.
// UUIDs are used internally in the tree.
static int sGlobalPaneId = 0;

Layout::Layout()
    : ownedTree_(std::make_unique<LayoutTree>()),
      tree_(ownedTree_.get())
{
    // Owned-tree path (tests). Create the root Container and set it as the
    // tree's root; the tree is purely this Layout's territory.
    subtreeRoot_ = tree_->createContainer(SplitDir::Horizontal);
    tree_->setRoot(subtreeRoot_);
}

Layout::Layout(LayoutTree* shared, Script::Engine* engine)
    : engine_(engine)
{
    if (shared) {
        tree_ = shared;
        // Shared-tree path (production). This Layout's subtree is an orphan
        // Container node in the shared tree; someone higher up (TabManager
        // when it lands the tab-stack unification) is responsible for
        // attaching it to a parent so JS / renderer can walk a rooted tree.
        subtreeRoot_ = tree_->createContainer(SplitDir::Horizontal);
    } else {
        ownedTree_ = std::make_unique<LayoutTree>();
        tree_ = ownedTree_.get();
        subtreeRoot_ = tree_->createContainer(SplitDir::Horizontal);
        tree_->setRoot(subtreeRoot_);
    }
}

Layout::~Layout()
{
    // The Terminals we hooked into Script::Engine's map live by Uuid keys.
    // We don't auto-extract them here — TabManager's closeTab / closePaneById
    // paths are responsible for moving Terminals into the graveyard with the
    // correct render-frame stamp BEFORE a Tab's Layout destructs. If a Layout
    // is destroyed without that prelude (e.g. plain delete outside TabManager),
    // the Terminals stay in the engine map with no owning Layout — which is
    // a controller bug, not something ~Layout can safely paper over (the
    // render thread may still be reading those pointers).
    //
    // Drop the subtree from whichever tree hosts us. For the owned-tree case
    // this is just cleanup (the tree is about to die anyway); for the shared
    // case this deletes the node and detaches from any parent that may have
    // attached it.
    if (tree_) tree_->destroyNode(subtreeRoot_);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int Layout::leftmostPaneIdInSubtree(Uuid root) const
{
    const Node* n = tree_->node(root);
    if (!n) return -1;
    // Terminal leaf → return its pane id.
    if (std::holds_alternative<TerminalData>(n->data)) {
        auto it = uuidToPaneId_.find(root);
        return it == uuidToPaneId_.end() ? -1 : it->second;
    }
    // Container or Stack → recurse into the first child (or active child).
    if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
        if (cd->children.empty()) return -1;
        return leftmostPaneIdInSubtree(cd->children.front().id);
    }
    if (const auto* sd = std::get_if<StackData>(&n->data)) {
        if (sd->activeChild.isNil()) return -1;
        return leftmostPaneIdInSubtree(sd->activeChild);
    }
    return -1;
}

void Layout::ratioToStretch(float ratio, bool /*newIsFirst*/,
                            int& newStretch, int& oldStretch) const
{
    // The "new" slot gets `ratio` share of the container, old gets 1-ratio.
    // newIsFirst affects positional order only, not size proportions.
    float r = std::clamp(ratio, 0.01f, 0.99f);
    int num = static_cast<int>(std::round(r * 100.0f));
    if (num < 1) num = 1;
    if (num > 99) num = 99;
    newStretch = num;
    oldStretch = 100 - num;
}

// ---------------------------------------------------------------------------
// Pane lifecycle
// ---------------------------------------------------------------------------

int Layout::createPane()
{
    int id = sGlobalPaneId++;

    // Only the very first createPane on an empty Layout actually inserts a
    // leaf; subsequent calls just allocate an ID for callers that haven't
    // built a pane yet (the pattern is createPane-then-insertTerminal for
    // the first pane, and splitPane for every one after that).
    Node* rootNode = tree_->node(subtreeRoot_);
    auto* rootData = rootNode ? std::get_if<ContainerData>(&rootNode->data) : nullptr;
    if (rootData && rootData->children.empty()) {
        Uuid u = tree_->createTerminal();
        tree_->appendChild(subtreeRoot_, ChildSlot{u, /*stretch=*/1});
        paneIdToUuid_[id] = u;
        uuidToPaneId_[u]  = id;
        mFocusedPaneId = id;
    }
    return id;
}

int Layout::splitPane(int paneId, LayoutNode::Dir dir, float ratio, bool newIsFirst)
{
    auto pit = paneIdToUuid_.find(paneId);
    if (pit == paneIdToUuid_.end()) {
        spdlog::warn("Layout::splitPane: pane {} not found", paneId);
        return -1;
    }
    Uuid target = pit->second;
    Node* targetNode = tree_->node(target);
    if (!targetNode) return -1;

    // Allocate the new pane's id + tree node up front so we can fail
    // cleanly without half-mutated state.
    int newId = sGlobalPaneId++;
    Uuid newPaneUuid = tree_->createTerminal();
    paneIdToUuid_[newId]         = newPaneUuid;
    uuidToPaneId_[newPaneUuid]   = newId;

    int newStretch = 1, oldStretch = 1;
    ratioToStretch(ratio, newIsFirst, newStretch, oldStretch);
    SplitDir sd = convertDir(dir);

    // Always wrap: create a new Container in the requested direction that
    // takes over the target's slot, then place the existing target + the
    // new pane as its children. Matches the binary-split tree's output
    // structure (every split = one internal node with exactly two leaves).
    Uuid wrapper = tree_->createContainer(sd);

    if (targetNode->parent.isNil()) {
        // Target is the root (subtreeRoot_'s direct sole child). Under the
        // fresh-Layout invariant the root is a Container with one Terminal
        // child; splitPane replaces that slot by wrapping.
        // But the wrapper becomes the root's sole child, not the new root —
        // we keep subtreeRoot_ fixed so outside code doesn't see the root
        // change out from under it.
        spdlog::warn("Layout::splitPane: target has no parent (shouldn't happen — subtreeRoot wraps)");
        return -1;
    }

    // Preserve the target's current slot properties (stretch, min/max, fixed)
    // on the wrapper so adjacent siblings keep their relative proportions.
    // Without this, a second split of a non-50/50-sized pane would collapse
    // the pane's ratio back to the default.
    ChildSlot wrapperSlot{wrapper, 1, 0, 0, 0};
    if (Node* parentMut = tree_->node(targetNode->parent)) {
        auto* cdMut = std::get_if<ContainerData>(&parentMut->data);
        if (cdMut) {
            for (const auto& s : cdMut->children) {
                if (s.id == target) {
                    wrapperSlot.stretch    = s.stretch;
                    wrapperSlot.minCells   = s.minCells;
                    wrapperSlot.maxCells   = s.maxCells;
                    wrapperSlot.fixedCells = s.fixedCells;
                    break;
                }
            }
        }
    }
    if (!tree_->replaceChild(targetNode->parent, target, wrapperSlot)) {
        spdlog::warn("Layout::splitPane: replaceChild failed");
        return -1;
    }

    // Now attach target + new leaf inside the wrapper, in the requested order.
    if (newIsFirst) {
        tree_->appendChild(wrapper, ChildSlot{newPaneUuid, newStretch});
        tree_->appendChild(wrapper, ChildSlot{target,      oldStretch});
    } else {
        tree_->appendChild(wrapper, ChildSlot{target,      oldStretch});
        tree_->appendChild(wrapper, ChildSlot{newPaneUuid, newStretch});
    }

    return newId;
}

int Layout::allocatePaneNode(Uuid* outNodeId)
{
    int id = sGlobalPaneId++;
    Uuid u = tree_->createTerminal();
    paneIdToUuid_[id] = u;
    uuidToPaneId_[u]  = id;
    if (outNodeId) *outNodeId = u;
    return id;
}

bool Layout::splitByNodeId(Uuid existingChildNodeId, LayoutNode::Dir dir,
                           Uuid newChildNodeId, bool newIsFirst)
{
    Node* target = tree_->node(existingChildNodeId);
    if (!target) return false;
    Node* incoming = tree_->node(newChildNodeId);
    if (!incoming) return false;
    if (target->parent.isNil()) return false;
    if (!incoming->parent.isNil()) return false; // must be orphan

    SplitDir sd = convertDir(dir);
    Uuid wrapper = tree_->createContainer(sd);

    // Inherit the target slot's sizing properties onto the wrapper so adjacent
    // siblings keep their relative proportions after the split (same logic as
    // splitPane — just UUID-in instead of uuid-allocated-here).
    ChildSlot wrapperSlot{wrapper, 1, 0, 0, 0};
    if (Node* parentMut = tree_->node(target->parent)) {
        if (auto* cd = std::get_if<ContainerData>(&parentMut->data)) {
            for (const auto& s : cd->children) {
                if (s.id == existingChildNodeId) {
                    wrapperSlot.stretch    = s.stretch;
                    wrapperSlot.minCells   = s.minCells;
                    wrapperSlot.maxCells   = s.maxCells;
                    wrapperSlot.fixedCells = s.fixedCells;
                    break;
                }
            }
        }
    }
    if (!tree_->replaceChild(target->parent, existingChildNodeId, wrapperSlot))
        return false;

    if (newIsFirst) {
        tree_->appendChild(wrapper, ChildSlot{newChildNodeId,      1});
        tree_->appendChild(wrapper, ChildSlot{existingChildNodeId, 1});
    } else {
        tree_->appendChild(wrapper, ChildSlot{existingChildNodeId, 1});
        tree_->appendChild(wrapper, ChildSlot{newChildNodeId,      1});
    }
    return true;
}

Terminal* Layout::insertTerminal(int paneId, std::unique_ptr<Terminal> t)
{
    if (!t) return nullptr;
    if (!engine_) {
        spdlog::error("Layout::insertTerminal: no Script::Engine configured; "
                      "Terminal for pane {} dropped", paneId);
        return nullptr;
    }
    auto uit = paneIdToUuid_.find(paneId);
    if (uit == paneIdToUuid_.end()) {
        spdlog::warn("Layout::insertTerminal: pane {} has no tree node", paneId);
        return nullptr;
    }
    t->setId(paneId);
    // Thread the tree-node UUID onto the Terminal so any downstream consumer
    // (ScriptEngine surfaces it as pane.nodeId) can find the node in the
    // shared tree without going back through Layout's id↔uuid map.
    t->setNodeId(uit->second);
    paneOrder_.push_back(paneId);
    return engine_->insertTerminal(uit->second, std::move(t));
}

std::unique_ptr<Terminal> Layout::extractPane(int paneId)
{
    if (paneOrder_.size() <= 1) {
        spdlog::warn("Layout::extractPane: cannot remove last pane");
        return nullptr;
    }

    auto pit = paneIdToUuid_.find(paneId);
    if (pit == paneIdToUuid_.end()) return nullptr;
    Uuid target = pit->second;
    Node* targetNode = tree_->node(target);
    if (!targetNode) return nullptr;
    Uuid parentUuid = targetNode->parent;
    if (parentUuid.isNil()) return nullptr; // root leaf, can't extract

    // Remove from tree.
    tree_->removeChild(parentUuid, target);
    tree_->destroyNode(target);
    paneIdToUuid_.erase(paneId);
    uuidToPaneId_.erase(target);

    // If the parent container now has exactly one child, collapse it by
    // replacing the parent with its remaining child (matching the binary
    // tree's "sibling takes over" behavior). Walk up as long as single-child
    // containers appear on the spine — but stop at subtreeRoot_ (root stays).
    Uuid walk = parentUuid;
    while (!walk.isNil() && walk != subtreeRoot_) {
        Node* n = tree_->node(walk);
        if (!n) break;
        auto* cd = std::get_if<ContainerData>(&n->data);
        if (!cd || cd->children.size() != 1) break;
        Uuid grand = n->parent;
        if (grand.isNil()) break;
        Uuid onlyChild = cd->children.front().id;
        // Preserve the sole child's slot stretch? Use stretch=1 on the
        // promotion slot to match the default sibling behavior.
        // Detach the only child from its current parent before replaceChild.
        tree_->removeChild(walk, onlyChild);
        if (!tree_->replaceChild(grand, walk, ChildSlot{onlyChild, 1})) {
            spdlog::warn("Layout::extractPane: collapse replaceChild failed");
            break;
        }
        tree_->destroyNode(walk);
        walk = grand;
    }

    // Remove from ordering index.
    paneOrder_.erase(std::remove(paneOrder_.begin(), paneOrder_.end(), paneId),
                     paneOrder_.end());

    // Focus handoff: if the extracted pane was focused, pick the first
    // remaining pane (matches today's "focus transfers to sibling" UX
    // approximately — the first paneOrder_ entry is a reasonable default).
    if (mFocusedPaneId == paneId) {
        mFocusedPaneId = paneOrder_.empty() ? -1 : paneOrder_.front();
    }
    if (mZoomedPaneId == paneId) {
        mZoomedPaneId = -1;
    }

    // Pull Terminal ownership back from the engine map. If the Terminal
    // was never inserted (engine_ missing or insertTerminal refused), this
    // returns nullptr — still correct, the caller just has nothing to
    // graveyard.
    if (engine_) return engine_->extractTerminal(target);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Lookup / focus / zoom
// ---------------------------------------------------------------------------

Terminal* Layout::pane(int paneId)
{
    if (!engine_) return nullptr;
    auto it = paneIdToUuid_.find(paneId);
    if (it == paneIdToUuid_.end()) return nullptr;
    return engine_->terminal(it->second);
}

std::vector<Terminal*> Layout::panes() const
{
    std::vector<Terminal*> out;
    if (!engine_) return out;
    out.reserve(paneOrder_.size());
    for (int id : paneOrder_) {
        auto uit = paneIdToUuid_.find(id);
        if (uit == paneIdToUuid_.end()) continue;
        if (Terminal* t = engine_->terminal(uit->second)) out.push_back(t);
    }
    return out;
}

PaneRect Layout::nodeRect(int paneId) const
{
    auto it = paneIdToUuid_.find(paneId);
    if (it == paneIdToUuid_.end()) return {};
    // Re-run layout with the last known framebuffer dims so callers can query
    // rects before their Terminal exists (splitPane-then-insertTerminal path).
    if (lastFbW_ == 0 || lastFbH_ == 0) return {};
    auto rects = tree_->computeRectsFrom(subtreeRoot_, LayoutRect{0, 0,
                                               static_cast<int>(lastFbW_),
                                               static_cast<int>(lastFbH_)},
                                    /*cellW=*/1, /*cellH=*/1);
    // Account for the tab bar + padding offset the same way computeRects does.
    // (This path is only called in the rare "read-pre-insert" flow; it's OK
    // to produce a slightly loose rect — TabManager uses it only for initial
    // Terminal sizing, which gets corrected on the next computeRects anyway.)
    auto rit = rects.find(it->second);
    if (rit == rects.end()) return {};
    return PaneRect{rit->second.x, rit->second.y, rit->second.w, rit->second.h};
}

void Layout::setFocusedPane(int id) { mFocusedPaneId = id; }

Terminal* Layout::focusedPane()
{
    if (mFocusedPaneId < 0) return nullptr;
    return pane(mFocusedPaneId);
}

void Layout::zoomPane(int id)
{
    // Toggle off if already zoomed on the same pane; otherwise zoom on `id`.
    if (mZoomedPaneId == id) mZoomedPaneId = -1;
    else                     mZoomedPaneId = id;
}

void Layout::unzoom() { mZoomedPaneId = -1; }

// ---------------------------------------------------------------------------
// Tab bar + rects
// ---------------------------------------------------------------------------

PaneRect Layout::tabBarRect(uint32_t windowW, uint32_t windowH) const
{
    if (tabBarHeight_ <= 0) return {};
    PaneRect r;
    r.w = static_cast<int>(windowW);
    r.h = tabBarHeight_;
    r.x = 0;
    r.y = (tabBarPosition_ == "top")
            ? 0
            : static_cast<int>(windowH) - tabBarHeight_;
    return r;
}

void Layout::computeRects(uint32_t windowW, uint32_t windowH)
{
    lastFbW_ = windowW;
    lastFbH_ = windowH;

    // Content area = full window minus tab bar.
    PaneRect content{0, 0, static_cast<int>(windowW), static_cast<int>(windowH)};
    if (tabBarHeight_ > 0) {
        if (tabBarPosition_ == "top") {
            content.y += tabBarHeight_;
            content.h -= tabBarHeight_;
        } else {
            content.h -= tabBarHeight_;
        }
    }
    if (content.w < 0) content.w = 0;
    if (content.h < 0) content.h = 0;

    // Zoom short-circuits: the zoomed pane gets the whole content rect; every
    // other pane's rect is zeroed (matches the old computeRects zoom path).
    if (mZoomedPaneId >= 0) {
        for (int id : paneOrder_) {
            Terminal* panePtr = pane(id);
            if (!panePtr) continue;
            if (id == mZoomedPaneId) {
                panePtr->setRect({content.x, content.y, content.w, content.h});
            } else {
                panePtr->setRect({0, 0, 0, 0});
            }
        }
        return;
    }

    // Normal path: run tree layout, then copy rects to each Terminal so every
    // existing reader of pane->rect() sees the right geometry.
    auto rects = tree_->computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h},
        /*cellW=*/1, /*cellH=*/1);

    for (int id : paneOrder_) {
        Terminal* panePtr = pane(id);
        if (!panePtr) continue;
        auto uit = paneIdToUuid_.find(id);
        if (uit == paneIdToUuid_.end()) { panePtr->setRect({0, 0, 0, 0}); continue; }
        auto rit = rects.find(uit->second);
        if (rit == rects.end()) { panePtr->setRect({0, 0, 0, 0}); continue; }
        panePtr->setRect(PaneRect{rit->second.x, rit->second.y, rit->second.w, rit->second.h});
    }
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

int Layout::paneAtPixel(int px, int py) const
{
    if (!engine_) return -1;
    for (int id : paneOrder_) {
        auto uit = paneIdToUuid_.find(id);
        if (uit == paneIdToUuid_.end()) continue;
        Terminal* panePtr = engine_->terminal(uit->second);
        if (!panePtr) continue;
        const PaneRect& r = panePtr->rect();
        if (r.isEmpty()) continue;
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
            return id;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Divider rects
// ---------------------------------------------------------------------------

std::vector<PaneRect> Layout::dividerRects(int dividerPixels) const
{
    auto pairs = dividersWithOwnerPanes(dividerPixels);
    std::vector<PaneRect> out;
    out.reserve(pairs.size());
    for (auto& p : pairs) out.push_back(p.second);
    return out;
}

std::vector<std::pair<int, PaneRect>>
Layout::dividersWithOwnerPanes(int dividerPixels) const
{
    if (dividerPixels <= 0 || paneOrder_.size() < 2) return {};
    // Walk the tree: for each Container, for each inter-child boundary, the
    // divider's "owner" is the leftmost/topmost leaf under the "first" child.
    std::vector<std::pair<int, PaneRect>> out;

    // Recompute rects (content-area not tab-bar-offset aware; callers pass
    // the already-correct lastFb dims). For the per-pane divider semantics,
    // we need the content rect; easiest: re-use computeRects' math.
    PaneRect content{0, 0, static_cast<int>(lastFbW_), static_cast<int>(lastFbH_)};
    if (tabBarHeight_ > 0) {
        if (tabBarPosition_ == "top") { content.y += tabBarHeight_; content.h -= tabBarHeight_; }
        else                          {                               content.h -= tabBarHeight_; }
    }
    auto rects = tree_->computeRectsFrom(subtreeRoot_, 
        LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);

    // Recursive walk with explicit first-leaf lookup.
    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = tree_->node(id);
        if (!n) return;
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
            if (cd->children.size() < 2 || dividerPixels <= 0) return;
            for (size_t i = 0; i + 1 < cd->children.size(); ++i) {
                auto a = rects.find(cd->children[i].id);
                if (a == rects.end()) continue;
                PaneRect divR;
                if (cd->dir == SplitDir::Horizontal) {
                    int splitX = a->second.x + a->second.w;
                    divR = {splitX, a->second.y, dividerPixels, a->second.h};
                } else {
                    int splitY = a->second.y + a->second.h;
                    divR = {a->second.x, splitY, a->second.w, dividerPixels};
                }
                int firstPaneId = leftmostPaneIdInSubtree(cd->children[i].id);
                out.push_back({firstPaneId, divR});
            }
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) walk(sd->activeChild);
        }
    };
    walk(subtreeRoot_);
    return out;
}

// ---------------------------------------------------------------------------
// Divider drag
// ---------------------------------------------------------------------------

bool Layout::resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta)
{
    auto pit = paneIdToUuid_.find(paneId);
    if (pit == paneIdToUuid_.end()) return false;
    Uuid target = pit->second;
    SplitDir want = convertDir(axis);

    // Walk up from the target to find the nearest ancestor Container whose
    // direction matches `axis`. Adjust stretch factors of the target's slot
    // and one neighbor so the edge shifts by pixelDelta.
    Uuid cur = target;
    while (true) {
        const Node* n = tree_->node(cur);
        if (!n || n->parent.isNil()) return false;
        const Node* p = tree_->node(n->parent);
        if (!p) return false;
        const auto* cd = std::get_if<ContainerData>(&p->data);
        if (!cd) return false;
        if (cd->dir != want) { cur = n->parent; continue; }

        // Find `cur` in the parent's children. Prefer the trailing boundary
        // (split with the next sibling). If cur is the last child, fall back
        // to the leading boundary (split with the previous sibling).
        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < cd->children.size(); ++i) {
            if (cd->children[i].id == cur) { idx = i; found = true; break; }
        }
        if (!found) return false;

        bool useTrailing = (idx + 1 < cd->children.size());
        size_t neighborIdx = useTrailing ? idx + 1 : idx - 1;
        if (!useTrailing && idx == 0) return false;

        // Convert pixelDelta into a proportional stretch shift using the
        // current pixel sizes of the two slots. New stretch ratios preserve
        // total size; we bump one by a few units and decrement the other.
        // Using the current slots' pixel dims requires computeRects-result;
        // we do a lightweight re-compute here to read them.
        PaneRect content{0, 0, static_cast<int>(lastFbW_), static_cast<int>(lastFbH_)};
        if (tabBarHeight_ > 0) {
            if (tabBarPosition_ == "top") { content.y += tabBarHeight_; content.h -= tabBarHeight_; }
            else                          {                               content.h -= tabBarHeight_; }
        }
        auto rects = tree_->computeRectsFrom(subtreeRoot_, 
            LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);

        auto ra = rects.find(cd->children[idx].id);
        auto rb = rects.find(cd->children[neighborIdx].id);
        if (ra == rects.end() || rb == rects.end()) return false;

        int axisA = (want == SplitDir::Horizontal) ? ra->second.w : ra->second.h;
        int axisB = (want == SplitDir::Horizontal) ? rb->second.w : rb->second.h;

        // Directional mapping: for useTrailing, pixelDelta > 0 grows slot A,
        // shrinks slot B. For leading, sign inverts.
        int signedDelta = useTrailing ? pixelDelta : -pixelDelta;
        int newA = std::max(1, axisA + signedDelta);
        int newB = std::max(1, axisB - signedDelta);

        // Mutate stretch factors directly on the child slots. Since the layout
        // engine uses integer stretch ratios, map new pixel sizes to
        // proportional stretch values. We keep the rest of the container's
        // children untouched.
        Node* pMut = tree_->node(n->parent);
        if (!pMut) return false;
        auto* cdMut = std::get_if<ContainerData>(&pMut->data);
        if (!cdMut) return false;
        cdMut->children[idx].stretch         = newA;
        cdMut->children[neighborIdx].stretch = newB;
        cdMut->children[idx].fixedCells      = 0;
        cdMut->children[neighborIdx].fixedCells = 0;
        return true;
    }
}
