#include "Tab.h"

#include "LayoutTree.h"
#include "Terminal.h"
#include "script/ScriptEngine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

Tab Tab::newSubtree(Script::Engine* engine)
{
    if (!engine) return {};
    // The tab is a Stack whose activeChild is the "content" Container
    // (holding the pane Terminals). Additional siblings may be added later
    // — e.g. by Action::ShowScrollback — and swapped in via setActiveChild
    // to produce what used to be the "overlay" UX. When the sibling's
    // Terminal exits, removeChild auto-retargets activeChild to the first
    // remaining child (the content Container), so the pane view returns.
    LayoutTree& tree = engine->layoutTree();
    Uuid stack   = tree.createStack();
    Uuid content = tree.createContainer(SplitDir::Horizontal);
    tree.appendChild(stack, ChildSlot{content, /*stretch=*/1});
    tree.setActiveChild(stack, content);
    return Tab{engine, stack};
}

namespace {

const std::string& kEmpty() {
    static const std::string s;
    return s;
}

SplitDir toSplitDir(LayoutNode::Dir d) {
    return d == LayoutNode::Dir::Horizontal ? SplitDir::Horizontal
                                             : SplitDir::Vertical;
}

// Resolve the pixel rect this tab's subtree should lay out into. In a fully
// tree-driven world the tab bar is a sibling node of the tabs Stack inside a
// top-level Container, so the tabs Stack's allocation already excludes the
// bar. Lookup preference:
//   1) If `subtreeRoot` is the active tab, it's in the root-level rect map.
//   2) Otherwise use the parent's rect (the tabs Stack) — all tabs share it.
//   3) Fallback: the full window (test harness, detached subtree).
PaneRect resolveSubtreeContentRect(const Script::Engine& eng, Uuid subtreeRoot)
{
    const LayoutTree& tree = eng.layoutTree();
    uint32_t fbW = eng.lastFbWidth();
    uint32_t fbH = eng.lastFbHeight();
    PaneRect fallback{0, 0, static_cast<int>(fbW), static_cast<int>(fbH)};
    if (fbW == 0 || fbH == 0) return fallback;

    if (tree.root().isNil()) return fallback;

    auto rects = tree.computeRects(LayoutRect{0, 0, (int)fbW, (int)fbH},
                                   eng.lastCellW(), eng.lastCellH());
    auto it = rects.find(subtreeRoot);
    if (it != rects.end()) {
        return PaneRect{it->second.x, it->second.y, it->second.w, it->second.h};
    }
    const Node* myNode = tree.node(subtreeRoot);
    if (myNode && !myNode->parent.isNil()) {
        auto parentIt = rects.find(myNode->parent);
        if (parentIt != rects.end()) {
            return PaneRect{parentIt->second.x, parentIt->second.y,
                            parentIt->second.w, parentIt->second.h};
        }
    }
    return fallback;
}

} // namespace

// ===========================================================================
// Title / icon
// ===========================================================================

const std::string& Tab::title() const
{
    if (!valid()) return kEmpty();
    const Node* n = eng_->layoutTree().node(subtreeRoot_);
    return n ? n->label : kEmpty();
}

void Tab::setTitle(const std::string& s)
{
    if (!valid()) return;
    eng_->layoutTree().setLabel(subtreeRoot_, s);
}

const std::string& Tab::icon() const
{
    if (!valid()) return kEmpty();
    return eng_->tabIcon(subtreeRoot_);
}

void Tab::setIcon(const std::string& s)
{
    if (!valid()) return;
    eng_->setTabIcon(subtreeRoot_, s);
}

// ===========================================================================
// Pane allocation + tree mutation
// ===========================================================================

Uuid Tab::createPane()
{
    if (!valid()) return {};
    LayoutTree& tree = eng_->layoutTree();

    // subtreeRoot is a Stack; its activeChild (first child, set by newSubtree)
    // is the content Container. Append the Terminal into the content Container
    // so the Stack can grow additional siblings (e.g. scrollback pager) later.
    Node* rootNode = tree.node(subtreeRoot_);
    if (!rootNode) return {};
    auto* stackData = std::get_if<StackData>(&rootNode->data);
    if (!stackData || stackData->children.empty()) {
        // Subtree was set up without a content Container — just allocate.
        return tree.createTerminal();
    }
    Uuid contentRoot = stackData->children.front().id;
    Node* contentNode = tree.node(contentRoot);
    if (!contentNode || !std::holds_alternative<ContainerData>(contentNode->data)) {
        return tree.createTerminal();
    }

    Uuid u = tree.createTerminal();
    tree.appendChild(contentRoot, ChildSlot{u, /*stretch=*/1});
    eng_->setFocusedTerminalNodeId(u);
    return u;
}

Uuid Tab::allocatePaneNode()
{
    if (!valid()) return {};
    return eng_->layoutTree().createTerminal();
}

bool Tab::splitByNodeId(Uuid existingChildNodeId, LayoutNode::Dir dir,
                        Uuid newChildNodeId, bool newIsFirst)
{
    if (!valid()) return false;
    return !eng_->layoutTree().splitByWrapping(existingChildNodeId,
                                                toSplitDir(dir),
                                                newChildNodeId,
                                                newIsFirst).isNil();
}

Terminal* Tab::insertTerminal(Uuid nodeId, std::unique_ptr<Terminal> t)
{
    if (!valid() || !t || nodeId.isNil()) return nullptr;
    t->setNodeId(nodeId);
    return eng_->insertTerminal(nodeId, std::move(t));
}

bool Tab::removeNodeSubtree(Uuid nodeId)
{
    if (!valid()) return false;
    if (nodeId == subtreeRoot_) return false; // use closeTab path
    return eng_->removeNodeSubtree(subtreeRoot_, nodeId);
}

// ===========================================================================
// Pane queries
// ===========================================================================

Terminal* Tab::pane(Uuid nodeId)
{
    if (!valid() || nodeId.isNil()) return nullptr;
    return eng_->layoutTree().contains(subtreeRoot_, nodeId) ? eng_->terminal(nodeId) : nullptr;
}

bool Tab::hasPaneSlot(Uuid nodeId) const
{
    if (!valid() || nodeId.isNil()) return false;
    return eng_->layoutTree().contains(subtreeRoot_, nodeId);
}

std::vector<Terminal*> Tab::panes() const
{
    std::vector<Terminal*> out;
    if (!valid()) return out;
    std::vector<Uuid> leaves;
    eng_->layoutTree().terminalLeavesIn(subtreeRoot_, /*onlyActiveStack=*/false, leaves);
    out.reserve(leaves.size());
    for (Uuid u : leaves) if (Terminal* t = eng_->terminal(u)) out.push_back(t);
    return out;
}

std::vector<Terminal*> Tab::activePanes() const
{
    std::vector<Terminal*> out;
    if (!valid()) return out;
    std::vector<Uuid> leaves;
    eng_->layoutTree().terminalLeavesIn(subtreeRoot_, /*onlyActiveStack=*/true, leaves);
    out.reserve(leaves.size());
    for (Uuid u : leaves) if (Terminal* t = eng_->terminal(u)) out.push_back(t);
    return out;
}

PaneRect Tab::nodeRect(Uuid nodeId) const
{
    if (!valid() || nodeId.isNil()) return {};
    PaneRect content = resolveSubtreeContentRect(*eng_, subtreeRoot_);
    if (content.isEmpty()) return {};
    auto rects = eng_->layoutTree().computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h},
        eng_->lastCellW(), eng_->lastCellH());
    auto it = rects.find(nodeId);
    if (it == rects.end()) return {};
    return PaneRect{it->second.x, it->second.y, it->second.w, it->second.h};
}

Uuid Tab::paneAtPixel(int px, int py) const
{
    if (!valid()) return {};
    for (Terminal* t : panes()) {
        if (!t) continue;
        const PaneRect& r = t->rect();
        if (r.isEmpty()) continue;
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
            return t->nodeId();
    }
    return {};
}

// ===========================================================================
// Focus (engine-wide, scoped to this tab for query semantics)
// ===========================================================================

Uuid Tab::focusedPaneId() const
{
    if (!valid()) return {};
    Uuid u = eng_->focusedTerminalNodeId();
    if (u.isNil()) return {};
    return eng_->layoutTree().contains(subtreeRoot_, u) ? u : Uuid{};
}

void Tab::setFocusedPane(Uuid nodeId)
{
    if (!valid()) return;
    eng_->setFocusedTerminalNodeId(nodeId);
}

Terminal* Tab::focusedPane()
{
    Uuid u = focusedPaneId();
    return u.isNil() ? nullptr : pane(u);
}

// ===========================================================================
// Global layout params (forwarded to Engine)
// ===========================================================================

void Tab::setDividerPixels(int px)   { if (valid()) eng_->setDividerPixels(px); }
int  Tab::dividerPixels() const      { return valid() ? eng_->dividerPixels() : 1; }

PaneRect Tab::tabBarRect(uint32_t windowW, uint32_t windowH) const
{
    if (!valid()) return {};
    Uuid bar = eng_->primaryTabBarNode();
    if (bar.isNil()) return {};
    const LayoutTree& tree = eng_->layoutTree();
    auto rects = tree.computeRects(LayoutRect{0, 0, (int)windowW, (int)windowH},
                                   eng_->lastCellW(), eng_->lastCellH());
    auto it = rects.find(bar);
    if (it == rects.end()) return {};
    return PaneRect{it->second.x, it->second.y, it->second.w, it->second.h};
}

// ===========================================================================
// Rect computation + dividers
// ===========================================================================

void Tab::computeRects(uint32_t windowW, uint32_t windowH, int cellW, int cellH)
{
    if (!valid()) return;
    eng_->setLastFramebuffer(windowW, windowH);
    eng_->setLastCellMetrics(cellW, cellH);
    PaneRect content = resolveSubtreeContentRect(*eng_, subtreeRoot_);

    // Zoom is tree-native (StackData::zoomTarget) — the tree routes the
    // Stack's rect to the zoom target and skips everything else, so
    // non-zoomed Terminals naturally fall through to the rect-map miss
    // below and get {0,0,0,0}.
    auto rects = eng_->layoutTree().computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h},
        eng_->lastCellW(), eng_->lastCellH());

    for (Terminal* t : panes()) {
        if (!t) continue;
        auto it = rects.find(t->nodeId());
        if (it == rects.end()) { t->setRect({0, 0, 0, 0}); continue; }
        t->setRect(PaneRect{it->second.x, it->second.y, it->second.w, it->second.h});
    }
}

std::vector<PaneRect> Tab::dividerRects(int dividerPixelsParam) const
{
    auto pairs = dividersWithOwnerPanes(dividerPixelsParam);
    std::vector<PaneRect> out;
    out.reserve(pairs.size());
    for (auto& p : pairs) out.push_back(p.second);
    return out;
}

std::vector<std::pair<Uuid, PaneRect>>
Tab::dividersWithOwnerPanes(int dividerPixelsParam) const
{
    if (!valid() || dividerPixelsParam <= 0) return {};
    // Dividers are only drawn between visible panes — inactive Stack
    // siblings (pager overlays, etc.) don't get rendered and thus can't
    // be divided from their neighbours.
    auto liveTerminals = activePanes();
    if (liveTerminals.size() < 2) return {};

    PaneRect content = resolveSubtreeContentRect(*eng_, subtreeRoot_);
    const LayoutTree& tree = eng_->layoutTree();
    auto rects = tree.computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h},
        eng_->lastCellW(), eng_->lastCellH());

    std::vector<std::pair<Uuid, LayoutRect>> raw;
    tree.dividersIn(subtreeRoot_, dividerPixelsParam, rects, raw);

    std::vector<std::pair<Uuid, PaneRect>> out;
    out.reserve(raw.size());
    for (const auto& [firstNode, r] : raw) {
        Uuid leafId = tree.leftmostTerminalIn(firstNode);
        out.push_back({leafId, PaneRect{r.x, r.y, r.w, r.h}});
    }
    return out;
}

bool Tab::resizePaneEdge(Uuid nodeId, LayoutNode::Dir axis, int pixelDelta)
{
    if (!valid() || nodeId.isNil()) return false;
    PaneRect content = resolveSubtreeContentRect(*eng_, subtreeRoot_);
    return eng_->layoutTree().resizeEdgeAlongAxis(
        nodeId, toSplitDir(axis), pixelDelta, subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h},
        eng_->lastCellW(), eng_->lastCellH());
}
