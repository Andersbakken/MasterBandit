#include "Tab.h"

#include "LayoutTree.h"
#include "Terminal.h"
#include "script/ScriptEngine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <functional>

// Global pane-id counter (was sGlobalPaneId in Layout.cpp). Preserved via
// Engine::allocatePaneId so paneIds stay globally unique across tabs.

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

// Small helpers migrated from Layout.cpp (private there).

int leftmostPaneIdInSubtree(const LayoutTree& tree,
                            const Script::Engine& eng,
                            Uuid root)
{
    const Node* n = tree.node(root);
    if (!n) return -1;
    if (std::holds_alternative<TerminalData>(n->data)) {
        return eng.paneIdForUuid(root);
    }
    if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
        if (cd->children.empty()) return -1;
        return leftmostPaneIdInSubtree(tree, eng, cd->children.front().id);
    }
    if (const auto* sd = std::get_if<StackData>(&n->data)) {
        if (sd->activeChild.isNil()) return -1;
        return leftmostPaneIdInSubtree(tree, eng, sd->activeChild);
    }
    return -1;
}

// Tab-bar-aware content rect used by computeRects / dividersWithOwnerPanes /
// resizePaneEdge. Mirrors the old Layout::computeRects early block.
PaneRect contentRectFromFb(uint32_t fbW, uint32_t fbH,
                            int tabBarHeight, const std::string& tabBarPosition)
{
    PaneRect content{0, 0, static_cast<int>(fbW), static_cast<int>(fbH)};
    if (tabBarHeight > 0) {
        if (tabBarPosition == "top") {
            content.y += tabBarHeight;
            content.h -= tabBarHeight;
        } else {
            content.h -= tabBarHeight;
        }
    }
    if (content.w < 0) content.w = 0;
    if (content.h < 0) content.h = 0;
    return content;
}

} // namespace

// ===========================================================================
// Title / icon / overlays
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

int Tab::createPane()
{
    if (!valid()) return -1;
    LayoutTree& tree = eng_->layoutTree();

    // subtreeRoot is a Stack; its activeChild (first child, set by newSubtree)
    // is the content Container. Append the Terminal into the content Container
    // so the Stack can grow additional siblings (e.g. scrollback pager) later.
    Node* rootNode = tree.node(subtreeRoot_);
    if (!rootNode) return -1;
    auto* stackData = std::get_if<StackData>(&rootNode->data);
    if (!stackData || stackData->children.empty()) {
        // Subtree was set up without a content Container — bail.
        return eng_->allocatePaneId();
    }
    Uuid contentRoot = stackData->children.front().id;
    Node* contentNode = tree.node(contentRoot);
    if (!contentNode || !std::holds_alternative<ContainerData>(contentNode->data)) {
        return eng_->allocatePaneId();
    }

    int id = eng_->allocatePaneId();
    Uuid u = tree.createTerminal();
    tree.appendChild(contentRoot, ChildSlot{u, /*stretch=*/1});
    eng_->registerPaneSlot(id, u);
    eng_->setFocusedTerminalNodeId(u);
    return id;
}

int Tab::allocatePaneNode(Uuid* outNodeId)
{
    if (!valid()) return -1;
    int id = eng_->allocatePaneId();
    Uuid u = eng_->layoutTree().createTerminal();
    eng_->registerPaneSlot(id, u);
    if (outNodeId) *outNodeId = u;
    return id;
}

bool Tab::splitByNodeId(Uuid existingChildNodeId, LayoutNode::Dir dir,
                        Uuid newChildNodeId, bool newIsFirst)
{
    if (!valid()) return false;
    LayoutTree& tree = eng_->layoutTree();
    Node* target = tree.node(existingChildNodeId);
    if (!target) return false;
    Node* incoming = tree.node(newChildNodeId);
    if (!incoming) return false;
    if (target->parent.isNil()) return false;
    if (!incoming->parent.isNil()) return false;

    SplitDir sd = toSplitDir(dir);
    Uuid wrapper = tree.createContainer(sd);

    ChildSlot wrapperSlot{wrapper, 1, 0, 0, 0};
    if (Node* parentMut = tree.node(target->parent)) {
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
    if (!tree.replaceChild(target->parent, existingChildNodeId, wrapperSlot))
        return false;

    if (newIsFirst) {
        tree.appendChild(wrapper, ChildSlot{newChildNodeId,       1});
        tree.appendChild(wrapper, ChildSlot{existingChildNodeId,  1});
    } else {
        tree.appendChild(wrapper, ChildSlot{existingChildNodeId,  1});
        tree.appendChild(wrapper, ChildSlot{newChildNodeId,       1});
    }
    return true;
}

Terminal* Tab::insertTerminal(int paneId, std::unique_ptr<Terminal> t)
{
    if (!valid() || !t) return nullptr;
    Uuid nodeId = eng_->uuidForPaneId(paneId);
    if (nodeId.isNil()) {
        spdlog::warn("Tab::insertTerminal: pane {} has no tree node", paneId);
        return nullptr;
    }
    t->setId(paneId);
    t->setNodeId(nodeId);
    return eng_->insertTerminal(nodeId, std::move(t));
}

bool Tab::removeNodeSubtree(Uuid nodeId)
{
    if (!valid()) return false;
    if (nodeId.isNil() || nodeId == subtreeRoot_) return false;
    LayoutTree& tree = eng_->layoutTree();
    Node* target = tree.node(nodeId);
    if (!target) return false;
    if (target->parent.isNil()) return false;

    // Walk the subtree, gather Terminal descendants. Guard: any live Terminal
    // in the engine map → refuse and leave the tree unchanged.
    std::vector<Uuid> terminalNodes;
    std::function<bool(Uuid)> walk = [&](Uuid id) -> bool {
        const Node* n = tree.node(id);
        if (!n) return true;
        if (n->kind() == NodeKind::Terminal) {
            if (eng_->terminal(id)) return false;
            terminalNodes.push_back(id);
            return true;
        }
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) if (!walk(s.id)) return false;
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            for (const auto& c : sd->children) if (!walk(c.id)) return false;
        }
        return true;
    };
    if (!walk(nodeId)) return false;

    // Strip paneId index entries for removed Terminals. Clear focus/zoom if
    // they pointed at the removed subtree.
    for (Uuid tn : terminalNodes) {
        int paneId = eng_->paneIdForUuid(tn);
        if (paneId >= 0) eng_->unregisterPaneSlot(paneId);
        if (eng_->focusedTerminalNodeId() == tn) eng_->setFocusedTerminalNodeId({});
        if (eng_->zoomedNodeId() == tn) eng_->setZoomedNodeId({});
    }
    if (eng_->zoomedNodeId() == nodeId) eng_->setZoomedNodeId({});

    Uuid parentUuid = target->parent;
    tree.removeChild(parentUuid, nodeId);
    tree.destroyNode(nodeId);

    // Collapse single-child Containers on the parent spine.
    Uuid cur = parentUuid;
    while (!cur.isNil() && cur != subtreeRoot_) {
        Node* n = tree.node(cur);
        if (!n) break;
        auto* cd = std::get_if<ContainerData>(&n->data);
        if (!cd || cd->children.size() != 1) break;
        Uuid grand = n->parent;
        if (grand.isNil()) break;
        Uuid onlyChild = cd->children.front().id;
        tree.removeChild(cur, onlyChild);
        if (!tree.replaceChild(grand, cur, ChildSlot{onlyChild, 1})) {
            spdlog::warn("Tab::removeNodeSubtree: collapse replaceChild failed");
            break;
        }
        tree.destroyNode(cur);
        cur = grand;
    }

    return true;
}

// ===========================================================================
// Pane queries
// ===========================================================================

Terminal* Tab::pane(int paneId)
{
    if (!valid()) return nullptr;
    Uuid u = eng_->uuidForPaneId(paneId);
    if (u.isNil()) return nullptr;
    // Scope to this tab: the slot must be inside subtreeRoot's subtree.
    // Cheap ancestor walk.
    const LayoutTree& tree = eng_->layoutTree();
    Uuid cur = u;
    while (!cur.isNil()) {
        if (cur == subtreeRoot_) return eng_->terminal(u);
        const Node* n = tree.node(cur);
        if (!n) return nullptr;
        cur = n->parent;
    }
    return nullptr;
}

bool Tab::hasPaneSlot(int paneId) const
{
    if (!valid()) return false;
    Uuid u = eng_->uuidForPaneId(paneId);
    if (u.isNil()) return false;
    const LayoutTree& tree = eng_->layoutTree();
    Uuid cur = u;
    while (!cur.isNil()) {
        if (cur == subtreeRoot_) return true;
        const Node* n = tree.node(cur);
        if (!n) return false;
        cur = n->parent;
    }
    return false;
}

std::vector<Terminal*> Tab::panes() const
{
    std::vector<Terminal*> out;
    if (!valid()) return out;
    const LayoutTree& tree = eng_->layoutTree();

    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = tree.node(id);
        if (!n) return;
        if (n->kind() == NodeKind::Terminal) {
            if (Terminal* t = eng_->terminal(id)) out.push_back(t);
            return;
        }
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            for (const auto& c : sd->children) walk(c.id);
        }
    };
    walk(subtreeRoot_);
    return out;
}

std::vector<Terminal*> Tab::activePanes() const
{
    std::vector<Terminal*> out;
    if (!valid()) return out;
    const LayoutTree& tree = eng_->layoutTree();

    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = tree.node(id);
        if (!n) return;
        if (n->kind() == NodeKind::Terminal) {
            if (Terminal* t = eng_->terminal(id)) out.push_back(t);
            return;
        }
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            // Only follow the activeChild so inactive siblings (e.g. the
            // content Container while a pager overlay is on top) don't
            // contribute to the visible-pane list.
            if (!sd->activeChild.isNil()) walk(sd->activeChild);
        }
    };
    walk(subtreeRoot_);
    return out;
}

PaneRect Tab::nodeRect(int paneId) const
{
    if (!valid()) return {};
    Uuid u = eng_->uuidForPaneId(paneId);
    if (u.isNil()) return {};
    uint32_t fbW = eng_->lastFbWidth();
    uint32_t fbH = eng_->lastFbHeight();
    if (fbW == 0 || fbH == 0) return {};
    PaneRect content = contentRectFromFb(fbW, fbH, eng_->tabBarHeight(),
                                          eng_->tabBarPosition());
    auto rects = eng_->layoutTree().computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);
    auto it = rects.find(u);
    if (it == rects.end()) return {};
    return PaneRect{it->second.x, it->second.y, it->second.w, it->second.h};
}

int Tab::paneAtPixel(int px, int py) const
{
    if (!valid()) return -1;
    for (Terminal* t : panes()) {
        if (!t) continue;
        const PaneRect& r = t->rect();
        if (r.isEmpty()) continue;
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
            return t->id();
    }
    return -1;
}

// ===========================================================================
// Focus / zoom (engine-wide, scoped to this tab for query semantics)
// ===========================================================================

int Tab::focusedPaneId() const
{
    if (!valid()) return -1;
    Uuid u = eng_->focusedTerminalNodeId();
    if (u.isNil()) return -1;
    // Scope to this tab.
    const LayoutTree& tree = eng_->layoutTree();
    Uuid cur = u;
    while (!cur.isNil()) {
        if (cur == subtreeRoot_) return eng_->paneIdForUuid(u);
        const Node* n = tree.node(cur);
        if (!n) return -1;
        cur = n->parent;
    }
    return -1;
}

void Tab::setFocusedPane(int paneId)
{
    if (!valid()) return;
    Uuid u = eng_->uuidForPaneId(paneId);
    if (u.isNil()) return;
    eng_->setFocusedTerminalNodeId(u);
}

Terminal* Tab::focusedPane()
{
    int id = focusedPaneId();
    return id < 0 ? nullptr : pane(id);
}

bool Tab::isZoomed() const
{
    return zoomedPaneId() >= 0;
}

int Tab::zoomedPaneId() const
{
    if (!valid()) return -1;
    Uuid u = eng_->zoomedNodeId();
    if (u.isNil()) return -1;
    const LayoutTree& tree = eng_->layoutTree();
    Uuid cur = u;
    while (!cur.isNil()) {
        if (cur == subtreeRoot_) return eng_->paneIdForUuid(u);
        const Node* n = tree.node(cur);
        if (!n) return -1;
        cur = n->parent;
    }
    return -1;
}

void Tab::zoomPane(int paneId)
{
    if (!valid()) return;
    Uuid u = eng_->uuidForPaneId(paneId);
    if (u.isNil()) return;
    if (eng_->zoomedNodeId() == u) eng_->setZoomedNodeId({});
    else                           eng_->setZoomedNodeId(u);
}

void Tab::unzoom()
{
    if (!valid()) return;
    eng_->setZoomedNodeId({});
}

// ===========================================================================
// Global layout params (forwarded to Engine)
// ===========================================================================

void Tab::setDividerPixels(int px)   { if (valid()) eng_->setDividerPixels(px); }
int  Tab::dividerPixels() const      { return valid() ? eng_->dividerPixels() : 1; }

void Tab::setTabBar(int height, const std::string& position)
{
    if (valid()) eng_->setTabBar(height, position);
}

PaneRect Tab::tabBarRect(uint32_t windowW, uint32_t windowH) const
{
    if (!valid() || eng_->tabBarHeight() <= 0) return {};
    PaneRect r;
    r.w = static_cast<int>(windowW);
    r.h = eng_->tabBarHeight();
    r.x = 0;
    r.y = (eng_->tabBarPosition() == "top")
            ? 0
            : static_cast<int>(windowH) - eng_->tabBarHeight();
    return r;
}

// ===========================================================================
// Rect computation + dividers
// ===========================================================================

void Tab::computeRects(uint32_t windowW, uint32_t windowH)
{
    if (!valid()) return;
    eng_->setLastFramebuffer(windowW, windowH);
    PaneRect content = contentRectFromFb(windowW, windowH, eng_->tabBarHeight(),
                                          eng_->tabBarPosition());

    int zoomPaneId = zoomedPaneId();
    if (zoomPaneId >= 0) {
        for (Terminal* t : panes()) {
            if (!t) continue;
            if (t->id() == zoomPaneId) {
                t->setRect({content.x, content.y, content.w, content.h});
            } else {
                t->setRect({0, 0, 0, 0});
            }
        }
        return;
    }

    auto rects = eng_->layoutTree().computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);

    for (Terminal* t : panes()) {
        if (!t) continue;
        Uuid u = eng_->uuidForPaneId(t->id());
        auto it = rects.find(u);
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

std::vector<std::pair<int, PaneRect>>
Tab::dividersWithOwnerPanes(int dividerPixelsParam) const
{
    if (!valid() || dividerPixelsParam <= 0) return {};
    // Dividers are only drawn between visible panes — inactive Stack
    // siblings (pager overlays, etc.) don't get rendered and thus can't
    // be divided from their neighbours.
    auto liveTerminals = activePanes();
    if (liveTerminals.size() < 2) return {};

    PaneRect content = contentRectFromFb(eng_->lastFbWidth(), eng_->lastFbHeight(),
                                          eng_->tabBarHeight(), eng_->tabBarPosition());
    const LayoutTree& tree = eng_->layoutTree();
    auto rects = tree.computeRectsFrom(subtreeRoot_,
        LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);

    std::vector<std::pair<int, PaneRect>> out;
    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = tree.node(id);
        if (!n) return;
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
            if (cd->children.size() < 2) return;
            for (size_t i = 0; i + 1 < cd->children.size(); ++i) {
                auto a = rects.find(cd->children[i].id);
                if (a == rects.end()) continue;
                PaneRect divR;
                if (cd->dir == SplitDir::Horizontal) {
                    int splitX = a->second.x + a->second.w;
                    divR = {splitX, a->second.y, dividerPixelsParam, a->second.h};
                } else {
                    int splitY = a->second.y + a->second.h;
                    divR = {a->second.x, splitY, a->second.w, dividerPixelsParam};
                }
                int firstPaneId = leftmostPaneIdInSubtree(tree, *eng_,
                                                           cd->children[i].id);
                out.push_back({firstPaneId, divR});
            }
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) walk(sd->activeChild);
        }
    };
    walk(subtreeRoot_);
    return out;
}

bool Tab::resizePaneEdge(int paneId, LayoutNode::Dir axis, int pixelDelta)
{
    if (!valid()) return false;
    LayoutTree& tree = eng_->layoutTree();
    Uuid target = eng_->uuidForPaneId(paneId);
    if (target.isNil()) return false;
    SplitDir want = toSplitDir(axis);

    Uuid cur = target;
    while (true) {
        const Node* n = tree.node(cur);
        if (!n || n->parent.isNil()) return false;
        const Node* p = tree.node(n->parent);
        if (!p) return false;
        const auto* cd = std::get_if<ContainerData>(&p->data);
        if (!cd) return false;
        if (cd->dir != want) { cur = n->parent; continue; }

        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < cd->children.size(); ++i) {
            if (cd->children[i].id == cur) { idx = i; found = true; break; }
        }
        if (!found) return false;

        bool useTrailing = (idx + 1 < cd->children.size());
        size_t neighborIdx = useTrailing ? idx + 1 : idx - 1;
        if (!useTrailing && idx == 0) return false;

        PaneRect content = contentRectFromFb(eng_->lastFbWidth(), eng_->lastFbHeight(),
                                              eng_->tabBarHeight(), eng_->tabBarPosition());
        auto rects = tree.computeRectsFrom(subtreeRoot_,
            LayoutRect{content.x, content.y, content.w, content.h}, 1, 1);

        auto ra = rects.find(cd->children[idx].id);
        auto rb = rects.find(cd->children[neighborIdx].id);
        if (ra == rects.end() || rb == rects.end()) return false;

        int axisA = (want == SplitDir::Horizontal) ? ra->second.w : ra->second.h;
        int axisB = (want == SplitDir::Horizontal) ? rb->second.w : rb->second.h;

        int signedDelta = useTrailing ? pixelDelta : -pixelDelta;
        int newA = std::max(1, axisA + signedDelta);
        int newB = std::max(1, axisB - signedDelta);

        Node* pMut = tree.node(n->parent);
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
