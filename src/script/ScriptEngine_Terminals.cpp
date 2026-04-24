// Engine's per-pane / per-tab ownership accessors, split out of
// ScriptEngine.cpp so code paths that link against Engine without pulling
// in QuickJS / libwebsockets (e.g. mb-tests) can resolve the symbols.
// Both mb and mb-tests link this TU.

#include "ScriptEngine.h"
#include "LayoutTree.h"
#include "Terminal.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>
#include <vector>

namespace Script {

namespace {
const std::string& kEmptyString() {
    static const std::string s;
    return s;
}
} // namespace

std::vector<Uuid> Engine::tabSubtreeRoots() const
{
    std::vector<Uuid> out;
    if (!layoutTree_) return out;
    const Node* rs = layoutTree_->node(layoutRootStack_);
    if (!rs) return out;
    const auto* sd = std::get_if<StackData>(&rs->data);
    if (!sd) return out;
    out.reserve(sd->children.size());
    for (const auto& s : sd->children) out.push_back(s.id);
    return out;
}

Uuid Engine::activeTabSubtreeRoot() const
{
    if (!layoutTree_) return {};
    const Node* rs = layoutTree_->node(layoutRootStack_);
    if (!rs) return {};
    const auto* sd = std::get_if<StackData>(&rs->data);
    return sd ? sd->activeChild : Uuid{};
}

int Engine::activeTabIndex() const
{
    Uuid active = activeTabSubtreeRoot();
    if (active.isNil()) return -1;
    auto roots = tabSubtreeRoots();
    for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
        if (roots[i] == active) return i;
    }
    return -1;
}

int Engine::tabCount() const
{
    if (!layoutTree_) return 0;
    const Node* rs = layoutTree_->node(layoutRootStack_);
    if (!rs) return 0;
    const auto* sd = std::get_if<StackData>(&rs->data);
    return sd ? static_cast<int>(sd->children.size()) : 0;
}

Uuid Engine::findTabSubtreeRootForNode(Uuid nodeId) const
{
    if (nodeId.isNil() || !layoutTree_) return {};
    // Any ancestor (inclusive) whose parent is the root Stack is the tab root.
    Uuid cur = nodeId;
    while (!cur.isNil()) {
        const Node* n = layoutTree_->node(cur);
        if (!n) return {};
        if (n->parent == layoutRootStack_) return cur;
        cur = n->parent;
    }
    return {};
}

bool Engine::removeNodeSubtree(Uuid scopeRoot, Uuid nodeId)
{
    if (nodeId.isNil() || scopeRoot.isNil()) return false;
    LayoutTree& tree = layoutTree();
    Node* target = tree.node(nodeId);
    if (!target) return false;
    if (target->parent.isNil()) return false;

    // Walk the subtree, gather Terminal descendants. Guard: any live Terminal
    // in the engine map → refuse and leave the tree unchanged.
    std::vector<Uuid> terminalNodes;
    tree.terminalLeavesIn(nodeId, /*onlyActiveStack=*/false, terminalNodes);
    for (Uuid u : terminalNodes) {
        if (terminal(u)) return false;
    }

    // Strip paneId index entries for removed Terminals. Clear focus if it
    // pointed at the removed subtree. Zoom state lives on StackData::zoomTarget
    // in the tree itself — destroyNode (called below) clears any Stack
    // pointing at a destroyed node, so no manual cleanup is needed here.
    for (Uuid tn : terminalNodes) {
        if (focusedTerminalNodeId() == tn) setFocusedTerminalNodeId({});
    }

    Uuid parentUuid = target->parent;
    tree.removeChild(parentUuid, nodeId);
    tree.destroyNode(nodeId);
    tree.collapseSingletonsAbove(parentUuid, scopeRoot);
    return true;
}

const std::unordered_map<Uuid, Rect, UuidHash>&
Engine::rootRects(uint32_t fbW, uint32_t fbH, int cellW, int cellH)
{
    const uint64_t fbKey    = (static_cast<uint64_t>(fbW) << 32) | fbH;
    const uint64_t treeRev  = layoutTree_->revision();
    const bool keyMatches = rootRectsKeyRevision_ == treeRev &&
                            rootRectsKeyFb_       == fbKey   &&
                            rootRectsKeyCellW_    == cellW   &&
                            rootRectsKeyCellH_    == cellH;
    if (!keyMatches) {
        rootRectsCache_ = layoutTree_->computeRects(
            Rect{0, 0, static_cast<int>(fbW), static_cast<int>(fbH)},
            cellW, cellH);
        rootRectsKeyFb_       = fbKey;
        rootRectsKeyCellW_    = cellW;
        rootRectsKeyCellH_    = cellH;
        rootRectsKeyRevision_ = treeRev;
    }
    return rootRectsCache_;
}

Uuid Engine::primaryTabBarNode() const
{
    // Find the first TabBar node in the tree by BFS from root. Typical
    // default-ui.js setup has exactly one; multiple TabBars are allowed
    // structurally but unusual. Returns nil if none exists (including the
    // empty-tree case before default-ui.js has run).
    if (!layoutTree_) return {};
    const LayoutTree& tree = *layoutTree_;
    Uuid r = tree.root();
    if (r.isNil()) return {};

    std::vector<Uuid> queue{r};
    while (!queue.empty()) {
        Uuid cur = queue.back();
        queue.pop_back();
        const Node* n = tree.node(cur);
        if (!n) continue;
        if (n->kind() == NodeKind::TabBar) return cur;
        std::visit([&](const auto& d) {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, ContainerData>) {
                for (const auto& s : d.children) queue.push_back(s.id);
            } else if constexpr (std::is_same_v<T, StackData>) {
                for (const auto& s : d.children) queue.push_back(s.id);
            }
        }, n->data);
    }
    return {};
}

// --- Terminal map (keyed by tree node UUID) --------------------------------

::Terminal* Engine::terminal(Uuid nodeId)
{
    auto it = terminals_.find(nodeId);
    return it == terminals_.end() ? nullptr : it->second.get();
}

const ::Terminal* Engine::terminal(Uuid nodeId) const
{
    auto it = terminals_.find(nodeId);
    return it == terminals_.end() ? nullptr : it->second.get();
}

::Terminal* Engine::insertTerminal(Uuid nodeId, std::unique_ptr<::Terminal> t)
{
    if (!t || nodeId.isNil()) return nullptr;
    ::Terminal* raw = t.get();
    terminals_[nodeId] = std::move(t);
    return raw;
}

std::unique_ptr<::Terminal> Engine::extractTerminal(Uuid nodeId)
{
    auto it = terminals_.find(nodeId);
    if (it == terminals_.end()) return nullptr;
    std::unique_ptr<::Terminal> out = std::move(it->second);
    terminals_.erase(it);
    return out;
}

// --- Per-tab icon map ------------------------------------------------------

const std::string& Engine::tabIcon(Uuid subtreeRoot) const
{
    auto it = tabIcons_.find(subtreeRoot);
    return it == tabIcons_.end() ? kEmptyString() : it->second;
}

void Engine::eraseTabIcon(Uuid subtreeRoot)
{
    tabIcons_.erase(subtreeRoot);
}

void Engine::eraseLastFocusedInTab(Uuid subtreeRoot)
{
    lastFocusedInTab_.erase(subtreeRoot);
}

void Engine::setFocusedTerminalNodeId(Uuid u)
{
    focusedTerminalNodeId_ = u;
    if (u.isNil()) return;
    // Record this pane as the "last focused" for whichever tab owns it, so
    // later tab-switches and inactive-tab reads (progress icon) can restore
    // the right pane. layoutTree_->contains walks the subtree; tab count is
    // small, so the linear scan is negligible.
    for (Uuid sub : tabSubtreeRoots()) {
        if (layoutTree_->contains(sub, u)) {
            lastFocusedInTab_[sub] = u;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-tab helpers (scoped to tab subtreeRoot)
// ---------------------------------------------------------------------------

namespace {

// Resolve the pixel rect this tab's subtree should lay out into. The tab bar
// is a sibling of the tabs Stack in a top-level Container, so the tabs
// Stack's allocation already excludes the bar. Lookup preference:
//   1) If `subtreeRoot` is the active tab, it's in the root-level rect map.
//   2) Otherwise use the parent's rect (the tabs Stack) — all tabs share it.
//   3) Fallback: the full window (test harness, detached subtree).
Rect resolveSubtreeContentRect(Engine& eng, Uuid subtreeRoot)
{
    const ::LayoutTree& tree = eng.layoutTree();
    uint32_t fbW = eng.lastFbWidth();
    uint32_t fbH = eng.lastFbHeight();
    Rect fallback{0, 0, static_cast<int>(fbW), static_cast<int>(fbH)};
    if (fbW == 0 || fbH == 0 || tree.root().isNil()) return fallback;

    const auto& rects = eng.rootRects(fbW, fbH, eng.lastCellW(), eng.lastCellH());
    auto it = rects.find(subtreeRoot);
    if (it != rects.end()) return it->second;
    const Node* myNode = tree.node(subtreeRoot);
    if (myNode && !myNode->parent.isNil()) {
        auto parentIt = rects.find(myNode->parent);
        if (parentIt != rects.end()) return parentIt->second;
    }
    return fallback;
}

const std::string& kEmptyStr() { static const std::string s; return s; }

} // namespace

const std::string& Engine::tabTitle(Uuid subtreeRoot) const
{
    if (subtreeRoot.isNil()) return kEmptyStr();
    const Node* n = layoutTree_->node(subtreeRoot);
    return n ? n->label : kEmptyStr();
}

Uuid Engine::createTabSubtree()
{
    // The tab is a Stack whose activeChild is the "content" Container
    // (holding the pane Terminals). Additional siblings may be added later
    // — e.g. by Action::ShowScrollback — and swapped in via setActiveChild
    // to produce what used to be the "overlay" UX.
    ::LayoutTree& tree = *layoutTree_;
    Uuid stack   = tree.createStack();
    Uuid content = tree.createContainer(SplitDir::Horizontal);
    tree.appendChild(stack, ChildSlot{content, /*stretch=*/1});
    tree.setActiveChild(stack, content);
    return stack;
}

Uuid Engine::createPaneInTab(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) return {};
    ::LayoutTree& tree = *layoutTree_;

    // subtreeRoot is a Stack; its activeChild (first child) is the content
    // Container. Append the Terminal into the content Container so the
    // Stack can grow additional siblings (pager overlays) later.
    Node* rootNode = tree.node(subtreeRoot);
    if (!rootNode) return {};
    auto* stackData = std::get_if<StackData>(&rootNode->data);
    if (!stackData || stackData->children.empty()) {
        return tree.createTerminal();
    }
    Uuid contentRoot = stackData->children.front().id;
    Node* contentNode = tree.node(contentRoot);
    if (!contentNode || !std::holds_alternative<ContainerData>(contentNode->data)) {
        return tree.createTerminal();
    }
    Uuid u = tree.createTerminal();
    tree.appendChild(contentRoot, ChildSlot{u, /*stretch=*/1});
    setFocusedTerminalNodeId(u);
    return u;
}

Uuid Engine::allocatePaneNode()
{
    return layoutTree_->createTerminal();
}

bool Engine::splitByNodeId(Uuid existingChildNodeId, SplitDir dir,
                           Uuid newChildNodeId, bool newIsFirst)
{
    return !layoutTree_->splitByWrapping(existingChildNodeId, dir,
                                         newChildNodeId, newIsFirst).isNil();
}

::Terminal* Engine::paneInSubtree(Uuid subtreeRoot, Uuid nodeId)
{
    if (subtreeRoot.isNil() || nodeId.isNil()) return nullptr;
    return layoutTree_->contains(subtreeRoot, nodeId) ? terminal(nodeId) : nullptr;
}

bool Engine::hasPaneSlotInSubtree(Uuid subtreeRoot, Uuid nodeId) const
{
    if (subtreeRoot.isNil() || nodeId.isNil()) return false;
    return layoutTree_->contains(subtreeRoot, nodeId);
}

std::vector<::Terminal*> Engine::panesInSubtree(Uuid subtreeRoot) const
{
    std::vector<::Terminal*> out;
    if (subtreeRoot.isNil()) return out;
    std::vector<Uuid> leaves;
    layoutTree_->terminalLeavesIn(subtreeRoot, /*onlyActiveStack=*/false, leaves);
    out.reserve(leaves.size());
    for (Uuid u : leaves) {
        auto it = terminals_.find(u);
        if (it != terminals_.end() && it->second) out.push_back(it->second.get());
    }
    return out;
}

std::vector<::Terminal*> Engine::activePanesInSubtree(Uuid subtreeRoot) const
{
    std::vector<::Terminal*> out;
    if (subtreeRoot.isNil()) return out;
    std::vector<Uuid> leaves;
    layoutTree_->terminalLeavesIn(subtreeRoot, /*onlyActiveStack=*/true, leaves);
    out.reserve(leaves.size());
    for (Uuid u : leaves) {
        auto it = terminals_.find(u);
        if (it != terminals_.end() && it->second) out.push_back(it->second.get());
    }
    return out;
}

Rect Engine::nodeRectInSubtree(Uuid subtreeRoot, Uuid nodeId) const
{
    if (subtreeRoot.isNil() || nodeId.isNil()) return {};
    Rect content = resolveSubtreeContentRect(const_cast<Engine&>(*this), subtreeRoot);
    if (content.isEmpty()) return {};
    auto rects = layoutTree_->computeRectsFrom(subtreeRoot, content,
                                               lastCellW(), lastCellH());
    auto it = rects.find(nodeId);
    if (it == rects.end()) return {};
    return it->second;
}

Uuid Engine::paneAtPixelInSubtree(Uuid subtreeRoot, int px, int py) const
{
    if (subtreeRoot.isNil()) return {};
    // Compute rects fresh — matches the semantics of every other spatial
    // query (nodeRectInSubtree, tabDividersWithOwnerPanes). Reading the
    // cached Terminal::rect() would be stale for background tabs when
    // hit-testing is called outside the per-frame cascade.
    Rect content = resolveSubtreeContentRect(const_cast<Engine&>(*this), subtreeRoot);
    auto rects = layoutTree_->computeRectsFrom(subtreeRoot, content,
                                               lastCellW(), lastCellH());
    for (::Terminal* t : activePanesInSubtree(subtreeRoot)) {
        if (!t) continue;
        auto it = rects.find(t->nodeId());
        if (it == rects.end()) continue;
        const Rect& r = it->second;
        if (r.isEmpty()) continue;
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
            return t->nodeId();
    }
    return {};
}

Uuid Engine::focusedPaneInSubtree(Uuid subtreeRoot) const
{
    if (subtreeRoot.isNil()) return {};
    Uuid u = focusedTerminalNodeId_;
    if (u.isNil()) return {};
    return layoutTree_->contains(subtreeRoot, u) ? u : Uuid{};
}

::Terminal* Engine::focusedTerminalInSubtree(Uuid subtreeRoot)
{
    Uuid u = focusedPaneInSubtree(subtreeRoot);
    return u.isNil() ? nullptr : paneInSubtree(subtreeRoot, u);
}

Uuid Engine::rememberedFocusInSubtree(Uuid subtreeRoot) const
{
    if (subtreeRoot.isNil()) return {};
    auto it = lastFocusedInTab_.find(subtreeRoot);
    if (it == lastFocusedInTab_.end()) return {};
    // Lazy validate: pane may have been removed since it was remembered.
    return layoutTree_->contains(subtreeRoot, it->second) ? it->second : Uuid{};
}

::Terminal* Engine::rememberedFocusTerminalInSubtree(Uuid subtreeRoot)
{
    Uuid u = rememberedFocusInSubtree(subtreeRoot);
    return u.isNil() ? nullptr : paneInSubtree(subtreeRoot, u);
}

Rect Engine::tabBarRect(uint32_t windowW, uint32_t windowH)
{
    Uuid bar = primaryTabBarNode();
    if (bar.isNil()) return {};
    const auto& rects = rootRects(windowW, windowH, lastCellW(), lastCellH());
    auto it = rects.find(bar);
    if (it == rects.end()) return {};
    return it->second;
}

void Engine::computeTabRects(Uuid subtreeRoot, uint32_t windowW, uint32_t windowH,
                             int cellW, int cellH)
{
    if (subtreeRoot.isNil()) return;
    setLastFramebuffer(windowW, windowH);
    setLastCellMetrics(cellW, cellH);
    Rect content = resolveSubtreeContentRect(*this, subtreeRoot);

    // Zoom is tree-native (StackData::zoomTarget); non-zoomed Terminals
    // fall through to the rect-map miss below and get {0,0,0,0}.
    auto rects = layoutTree_->computeRectsFrom(subtreeRoot, content,
                                               lastCellW(), lastCellH());

    for (::Terminal* t : panesInSubtree(subtreeRoot)) {
        if (!t) continue;
        auto it = rects.find(t->nodeId());
        if (it == rects.end()) { t->setRect({0, 0, 0, 0}); continue; }
        t->setRect(it->second);
    }
}

std::vector<Rect>
Engine::tabDividerRects(Uuid subtreeRoot, int dividerPx) const
{
    auto pairs = tabDividersWithOwnerPanes(subtreeRoot, dividerPx);
    std::vector<Rect> out;
    out.reserve(pairs.size());
    for (auto& p : pairs) out.push_back(p.second);
    return out;
}

std::vector<std::pair<Uuid, Rect>>
Engine::tabDividersWithOwnerPanes(Uuid subtreeRoot, int dividerPx) const
{
    if (subtreeRoot.isNil() || dividerPx <= 0) return {};
    auto liveTerminals = activePanesInSubtree(subtreeRoot);
    if (liveTerminals.size() < 2) return {};

    Rect content = resolveSubtreeContentRect(const_cast<Engine&>(*this),
                                                    subtreeRoot);
    const ::LayoutTree& tree = *layoutTree_;
    auto rects = tree.computeRectsFrom(subtreeRoot, content,
                                        lastCellW(), lastCellH());

    std::vector<std::pair<Uuid, Rect>> raw;
    tree.dividersIn(subtreeRoot, dividerPx, rects, raw);

    std::vector<std::pair<Uuid, Rect>> out;
    out.reserve(raw.size());
    for (const auto& [firstNode, r] : raw) {
        Uuid leafId = tree.leftmostTerminalIn(firstNode);
        out.push_back({leafId, r});
    }
    return out;
}

bool Engine::resizeTabPaneEdge(Uuid subtreeRoot, Uuid nodeId,
                               SplitDir axis, int pixelDelta)
{
    if (subtreeRoot.isNil() || nodeId.isNil()) return false;
    Rect content = resolveSubtreeContentRect(*this, subtreeRoot);
    return layoutTree_->resizeEdgeAlongAxis(
        nodeId, axis, pixelDelta, subtreeRoot,
        content, lastCellW(), lastCellH());
}

} // namespace Script
