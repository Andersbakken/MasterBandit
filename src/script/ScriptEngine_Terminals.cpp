// Engine's per-pane / per-tab ownership accessors, split out of
// ScriptEngine.cpp so Layout.cpp + Tab.cpp (which mb-tests links directly
// without pulling in QuickJS / libwebsockets) can resolve the symbols.
// Both mb and mb-tests link this TU.

#include "ScriptEngine.h"
#include "Layout.h"
#include "LayoutTree.h"
#include "Terminal.h"

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

void Engine::setTabIcon(Uuid subtreeRoot, const std::string& s)
{
    if (subtreeRoot.isNil()) return;
    tabIcons_[subtreeRoot] = s;
}

void Engine::eraseTabIcon(Uuid subtreeRoot)
{
    tabIcons_.erase(subtreeRoot);
}

} // namespace Script
