#include "LayoutTree.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <type_traits>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<ChildSlot>* childrenOf(Node* n)
{
    if (!n) return nullptr;
    return std::visit([](auto& d) -> std::vector<ChildSlot>* {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, ContainerData>) return &d.children;
        else if constexpr (std::is_same_v<T, StackData>) return &d.children;
        else return nullptr;
    }, n->data);
}

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

Uuid LayoutTree::createTerminal()
{
    Uuid id = Uuid::generate();
    auto n = std::make_unique<Node>();
    n->id = id;
    n->data = TerminalData{};
    nodes_.emplace(id, std::move(n));
    return id;
}

Uuid LayoutTree::createContainer(SplitDir dir)
{
    Uuid id = Uuid::generate();
    auto n = std::make_unique<Node>();
    n->id = id;
    n->data = ContainerData{ dir, {} };
    nodes_.emplace(id, std::move(n));
    return id;
}

Uuid LayoutTree::createStack()
{
    Uuid id = Uuid::generate();
    auto n = std::make_unique<Node>();
    n->id = id;
    n->data = StackData{};
    nodes_.emplace(id, std::move(n));
    return id;
}

Uuid LayoutTree::createTabBar()
{
    Uuid id = Uuid::generate();
    auto n = std::make_unique<Node>();
    n->id = id;
    n->data = TabBarData{};
    nodes_.emplace(id, std::move(n));
    return id;
}

// ---------------------------------------------------------------------------
// Root / lookup
// ---------------------------------------------------------------------------

bool LayoutTree::setRoot(Uuid id)
{
    Node* n = node(id);
    if (!n) {
        spdlog::warn("LayoutTree::setRoot: unknown node {}", id.toString());
        return false;
    }
    if (!n->parent.isNil()) {
        spdlog::warn("LayoutTree::setRoot: node {} already has parent {}",
                     id.toString(), n->parent.toString());
        return false;
    }
    root_ = id;
    dirty_ = true;
    return true;
}

Node* LayoutTree::node(Uuid id)
{
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

const Node* LayoutTree::node(Uuid id) const
{
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

bool LayoutTree::contains(Uuid ancestor, Uuid descendant) const
{
    if (ancestor.isNil() || descendant.isNil()) return false;
    Uuid cur = descendant;
    while (!cur.isNil()) {
        if (cur == ancestor) return true;
        const Node* n = node(cur);
        if (!n) return false;
        cur = n->parent;
    }
    return false;
}

Uuid LayoutTree::nearestAncestorOfKind(Uuid start, NodeKind kind) const
{
    Uuid cur = start;
    while (!cur.isNil()) {
        const Node* n = node(cur);
        if (!n) return {};
        if (n->kind() == kind) return cur;
        cur = n->parent;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Child management
// ---------------------------------------------------------------------------

bool LayoutTree::appendChild(Uuid parent, ChildSlot slot)
{
    Node* p = node(parent);
    if (!p) {
        spdlog::warn("LayoutTree::appendChild: parent {} not found", parent.toString());
        return false;
    }
    auto* kids = childrenOf(p);
    if (!kids) {
        spdlog::warn("LayoutTree::appendChild: parent {} is a leaf (not Container/Stack)",
                     parent.toString());
        return false;
    }
    Node* c = node(slot.id);
    if (!c) {
        spdlog::warn("LayoutTree::appendChild: child {} not found", slot.id.toString());
        return false;
    }
    if (!c->parent.isNil()) {
        spdlog::warn("LayoutTree::appendChild: child {} already has parent {}",
                     slot.id.toString(), c->parent.toString());
        return false;
    }
    c->parent = parent;
    kids->push_back(slot);

    // Stack: if no active child set yet, the first appended becomes active.
    if (auto* sd = std::get_if<StackData>(&p->data); sd && sd->activeChild.isNil()) {
        sd->activeChild = slot.id;
    }
    dirty_ = true;
    return true;
}

bool LayoutTree::removeChild(Uuid parent, Uuid child)
{
    Node* p = node(parent);
    if (!p) return false;
    auto* kids = childrenOf(p);
    if (!kids) return false;

    auto it = std::find_if(kids->begin(), kids->end(),
                           [&](const ChildSlot& s) { return s.id == child; });
    if (it == kids->end()) return false;
    kids->erase(it);

    Node* c = node(child);
    if (c) c->parent = {};

    if (auto* sd = std::get_if<StackData>(&p->data); sd && sd->activeChild == child) {
        sd->activeChild = kids->empty() ? Uuid{} : kids->front().id;
    }
    dirty_ = true;
    return true;
}

bool LayoutTree::replaceChild(Uuid parent, Uuid oldChild, ChildSlot newSlot)
{
    Node* p = node(parent);
    if (!p) return false;
    auto* kids = childrenOf(p);
    if (!kids) return false;

    auto it = std::find_if(kids->begin(), kids->end(),
                           [&](const ChildSlot& s) { return s.id == oldChild; });
    if (it == kids->end()) return false;

    Node* newNode = node(newSlot.id);
    if (!newNode) return false;
    if (!newNode->parent.isNil()) {
        spdlog::warn("LayoutTree::replaceChild: new node {} already has parent",
                     newSlot.id.toString());
        return false;
    }

    // Detach old, attach new in the same slot position.
    Node* oldNode = node(oldChild);
    if (oldNode) oldNode->parent = {};
    *it = newSlot;
    newNode->parent = parent;

    if (auto* sd = std::get_if<StackData>(&p->data); sd && sd->activeChild == oldChild) {
        sd->activeChild = newSlot.id;
    }
    dirty_ = true;
    return true;
}

bool LayoutTree::setStackZoom(Uuid stack, Uuid target)
{
    Node* s = node(stack);
    if (!s) return false;
    auto* sd = std::get_if<StackData>(&s->data);
    if (!sd) {
        spdlog::warn("LayoutTree::setStackZoom: node {} is not a Stack", stack.toString());
        return false;
    }
    if (target.isNil()) {
        sd->zoomTarget = {};
        dirty_ = true;
        return true;
    }
    if (target == stack) {
        spdlog::warn("LayoutTree::setStackZoom: target cannot be the stack itself");
        return false;
    }
    if (!contains(stack, target)) {
        spdlog::warn("LayoutTree::setStackZoom: target {} not in stack {}'s subtree",
                     target.toString(), stack.toString());
        return false;
    }
    sd->zoomTarget = target;
    dirty_ = true;
    return true;
}

bool LayoutTree::setActiveChild(Uuid stack, Uuid child)
{
    Node* s = node(stack);
    if (!s) return false;
    auto* sd = std::get_if<StackData>(&s->data);
    if (!sd) {
        spdlog::warn("LayoutTree::setActiveChild: node {} is not a Stack", stack.toString());
        return false;
    }
    auto it = std::find_if(sd->children.begin(), sd->children.end(),
                           [&](const ChildSlot& s) { return s.id == child; });
    if (it == sd->children.end()) {
        spdlog::warn("LayoutTree::setActiveChild: {} is not a child of stack {}",
                     child.toString(), stack.toString());
        return false;
    }
    sd->activeChild = child;
    dirty_ = true;
    return true;
}

bool LayoutTree::setTabBarStack(Uuid tabBar, Uuid stack)
{
    Node* b = node(tabBar);
    if (!b) return false;
    auto* bd = std::get_if<TabBarData>(&b->data);
    if (!bd) {
        spdlog::warn("LayoutTree::setTabBarStack: node {} is not a TabBar", tabBar.toString());
        return false;
    }
    // Validate target: either nil (clear the binding) or an existing Stack.
    if (!stack.isNil()) {
        Node* s = node(stack);
        if (!s) {
            spdlog::warn("LayoutTree::setTabBarStack: stack {} not found", stack.toString());
            return false;
        }
        if (!std::holds_alternative<StackData>(s->data)) {
            spdlog::warn("LayoutTree::setTabBarStack: node {} is not a Stack", stack.toString());
            return false;
        }
    }
    bd->boundStack = stack;
    return true;
}

void LayoutTree::setLabel(Uuid id, std::string label)
{
    if (Node* n = node(id)) n->label = std::move(label);
}

namespace {
ChildSlot* findSlot(std::vector<ChildSlot>* kids, Uuid child)
{
    if (!kids) return nullptr;
    for (auto& s : *kids) if (s.id == child) return &s;
    return nullptr;
}
} // namespace

bool LayoutTree::setSlotStretch(Uuid parent, Uuid child, int stretch)
{
    Node* p = node(parent); if (!p) return false;
    ChildSlot* s = findSlot(childrenOf(p), child);
    if (!s) return false;
    s->stretch = std::max(0, stretch);
    dirty_ = true;
    return true;
}

bool LayoutTree::setSlotMinCells(Uuid parent, Uuid child, int minCells)
{
    Node* p = node(parent); if (!p) return false;
    ChildSlot* s = findSlot(childrenOf(p), child);
    if (!s) return false;
    s->minCells = std::max(0, minCells);
    dirty_ = true;
    return true;
}

bool LayoutTree::setSlotMaxCells(Uuid parent, Uuid child, int maxCells)
{
    Node* p = node(parent); if (!p) return false;
    ChildSlot* s = findSlot(childrenOf(p), child);
    if (!s) return false;
    s->maxCells = std::max(0, maxCells);
    dirty_ = true;
    return true;
}

bool LayoutTree::setSlotFixedCells(Uuid parent, Uuid child, int fixedCells)
{
    Node* p = node(parent); if (!p) return false;
    ChildSlot* s = findSlot(childrenOf(p), child);
    if (!s) return false;
    s->fixedCells = std::max(0, fixedCells);
    dirty_ = true;
    return true;
}

void LayoutTree::destroyNode(Uuid id)
{
    Node* n = node(id);
    if (!n) return;

    // Recurse into children first so destruction happens bottom-up.
    if (auto* kids = childrenOf(n)) {
        // Copy the child IDs because destroyNode mutates the parent's list
        // via removeChild -> erase, invalidating iterators.
        std::vector<Uuid> ids;
        ids.reserve(kids->size());
        for (const auto& s : *kids) ids.push_back(s.id);
        for (Uuid cid : ids) destroyNode(cid);
    }

    // Detach from parent.
    if (!n->parent.isNil()) {
        removeChild(n->parent, id);
    }

    if (root_ == id) root_ = Uuid{};
    nodes_.erase(id);
    // Clear any Stack zoomTarget pointing at this destroyed node. Ancestor
    // Stacks are likeliest but any Stack in the tree could technically point
    // here if the node was nested under it. Mirrors the activeChild retarget
    // in removeChild.
    for (auto& [_, np] : nodes_) {
        if (auto* sd = std::get_if<StackData>(&np->data)) {
            if (sd->zoomTarget == id) sd->zoomTarget = {};
        }
    }
    dirty_ = true;
    // Any TabBar referencing this node becomes dangling; that's intentional —
    // computeRects ignores TabBar.boundStack entirely, so render simply sees
    // an empty bar. Sweeping is the renderer's job on its next frame.
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void layoutSubtree(const LayoutTree& tree, Uuid id, LayoutRect rect,
                          int cellW, int cellH,
                          std::unordered_map<Uuid, LayoutRect, UuidHash>& out)
{
    const Node* n = tree.node(id);
    if (!n) return;
    out[id] = rect;

    std::visit([&](const auto& d) {
        using T = std::decay_t<decltype(d)>;

        if constexpr (std::is_same_v<T, ContainerData>) {
            const bool horizontal = d.dir == SplitDir::Horizontal;
            const int available = horizontal ? rect.w : rect.h;
            int cellSize = horizontal ? cellW : cellH;
            if (cellSize <= 0) cellSize = 1;

            const size_t n = d.children.size();
            if (n == 0) return;

            std::vector<int>  sizes(n, 0);
            std::vector<int>  mins(n, 0);
            std::vector<bool> fixed(n, false);
            int totalFixed   = 0;
            int totalStretch = 0;

            for (size_t i = 0; i < n; ++i) {
                const auto& s = d.children[i];
                mins[i] = std::max(0, s.minCells) * cellSize;
                if (s.fixedCells > 0) {
                    sizes[i] = s.fixedCells * cellSize;
                    totalFixed += sizes[i];
                    fixed[i] = true;
                } else {
                    totalStretch += std::max(0, s.stretch);
                }
            }

            int remaining = available - totalFixed;
            if (remaining < 0) remaining = 0;

            if (totalStretch > 0 && remaining > 0) {
                for (size_t i = 0; i < n; ++i) {
                    if (fixed[i]) continue;
                    const auto& s = d.children[i];
                    int want = static_cast<int>(
                        (static_cast<long long>(remaining) *
                         std::max(0, s.stretch)) / totalStretch);
                    if (s.maxCells > 0) {
                        int maxPx = s.maxCells * cellSize;
                        if (want > maxPx) want = maxPx;
                    }
                    if (want < mins[i]) want = mins[i];
                    sizes[i] = want;
                }
            } else {
                // No stretch budget: flexible children collapse to min.
                for (size_t i = 0; i < n; ++i) {
                    if (!fixed[i]) sizes[i] = mins[i];
                }
            }

            // --- Overflow policy: shrink trailing children to reclaim excess,
            // walking backward until the excess is zero. Each child shrinks
            // by at most its own current size (going to zero counts as
            // "clipped"). If excess is still > 0 after zeroing everything
            // that's "shrink-below-min" against the only remaining nonzero
            // child — which can only happen if the first child's own size
            // already exceeds the window, and we stop there. ---
            int total = 0;
            for (int s : sizes) total += s;
            if (total > available) {
                int excess = total - available;
                for (size_t i = n; i-- > 0 && excess > 0; ) {
                    int take = std::min(sizes[i], excess);
                    sizes[i] -= take;
                    excess   -= take;
                }
            } else if (total < available) {
                // Distribute leftover to the last flexible, nonzero-stretch
                // child. This keeps pixel-perfect rect sums without relying
                // on float division rounding behavior.
                int leftover = available - total;
                for (size_t i = n; i-- > 0; ) {
                    if (!fixed[i] && d.children[i].stretch > 0) {
                        sizes[i] += leftover;
                        break;
                    }
                }
            }

            int cursor = horizontal ? rect.x : rect.y;
            for (size_t i = 0; i < n; ++i) {
                LayoutRect r = rect;
                if (horizontal) { r.x = cursor; r.w = sizes[i]; }
                else            { r.y = cursor; r.h = sizes[i]; }
                cursor += sizes[i];
                if (!r.isEmpty()) {
                    layoutSubtree(tree, d.children[i].id, r, cellW, cellH, out);
                }
            }
        }
        else if constexpr (std::is_same_v<T, StackData>) {
            // Zoom override: if set, the Stack's entire rect goes to the zoom
            // target instead of activeChild. All non-zoom descendants are
            // skipped this frame, so TIOCSWINSZ/divider/hit-test paths see
            // them as hidden (rect map miss).
            if (!d.zoomTarget.isNil()) {
                layoutSubtree(tree, d.zoomTarget, rect, cellW, cellH, out);
            } else if (!d.activeChild.isNil()) {
                layoutSubtree(tree, d.activeChild, rect, cellW, cellH, out);
            }
        }
        // Terminal / TabBar: leaves; already emitted their own rect above.
    }, n->data);
}

std::unordered_map<Uuid, LayoutRect, UuidHash> LayoutTree::computeRects(
    LayoutRect window, int cellW, int cellH) const
{
    return computeRectsFrom(root_, window, cellW, cellH);
}

std::unordered_map<Uuid, LayoutRect, UuidHash> LayoutTree::computeRectsFrom(
    Uuid start, LayoutRect window, int cellW, int cellH) const
{
    std::unordered_map<Uuid, LayoutRect, UuidHash> out;
    if (start.isNil() || window.isEmpty()) return out;
    layoutSubtree(*this, start, window, cellW, cellH, out);
    return out;
}

void LayoutTree::dividersIn(Uuid start, int dividerPixels,
                            const std::unordered_map<Uuid, LayoutRect, UuidHash>& rects,
                            std::vector<std::pair<Uuid, LayoutRect>>& out) const
{
    if (start.isNil() || dividerPixels <= 0) return;

    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = node(id);
        if (!n) return;
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
            if (cd->children.size() < 2) return;
            for (size_t i = 0; i + 1 < cd->children.size(); ++i) {
                auto a = rects.find(cd->children[i].id);
                if (a == rects.end()) continue;
                LayoutRect divR;
                if (cd->dir == SplitDir::Horizontal) {
                    int splitX = a->second.x + a->second.w;
                    divR = {splitX, a->second.y, dividerPixels, a->second.h};
                } else {
                    int splitY = a->second.y + a->second.h;
                    divR = {a->second.x, splitY, a->second.w, dividerPixels};
                }
                out.push_back({cd->children[i].id, divR});
            }
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) walk(sd->activeChild);
        }
    };
    walk(start);
}

Uuid LayoutTree::splitByWrapping(Uuid existingChild, SplitDir dir,
                                  Uuid newChild, bool newIsFirst)
{
    Node* target = node(existingChild);
    if (!target) return {};
    Node* incoming = node(newChild);
    if (!incoming) return {};
    if (target->parent.isNil()) return {};
    if (!incoming->parent.isNil()) return {};

    Uuid wrapper = createContainer(dir);

    // Inherit the existing slot's sizing knobs so the parent's allocation
    // doesn't lurch on split.
    ChildSlot wrapperSlot{wrapper, 1, 0, 0, 0};
    if (Node* parentMut = node(target->parent)) {
        if (auto* cd = std::get_if<ContainerData>(&parentMut->data)) {
            for (const auto& s : cd->children) {
                if (s.id == existingChild) {
                    wrapperSlot.stretch    = s.stretch;
                    wrapperSlot.minCells   = s.minCells;
                    wrapperSlot.maxCells   = s.maxCells;
                    wrapperSlot.fixedCells = s.fixedCells;
                    break;
                }
            }
        }
    }
    if (!replaceChild(target->parent, existingChild, wrapperSlot))
        return {};

    if (newIsFirst) {
        appendChild(wrapper, ChildSlot{newChild,      1});
        appendChild(wrapper, ChildSlot{existingChild, 1});
    } else {
        appendChild(wrapper, ChildSlot{existingChild, 1});
        appendChild(wrapper, ChildSlot{newChild,      1});
    }
    return wrapper;
}

bool LayoutTree::resizeEdgeAlongAxis(Uuid target, SplitDir axis, int pixelDelta,
                                      Uuid ancestorRoot, LayoutRect window,
                                      int cellW, int cellH)
{
    if (target.isNil() || ancestorRoot.isNil()) return false;

    // Walk up until we find a Container parent of the requested split dir.
    Uuid cur = target;
    while (true) {
        const Node* n = node(cur);
        if (!n || n->parent.isNil()) return false;
        const Node* p = node(n->parent);
        if (!p) return false;
        const auto* cd = std::get_if<ContainerData>(&p->data);
        if (!cd) return false;
        if (cd->dir != axis) { cur = n->parent; continue; }

        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < cd->children.size(); ++i) {
            if (cd->children[i].id == cur) { idx = i; found = true; break; }
        }
        if (!found) return false;

        bool useTrailing = (idx + 1 < cd->children.size());
        size_t neighborIdx = useTrailing ? idx + 1 : idx - 1;
        if (!useTrailing && idx == 0) return false;

        auto rects = computeRectsFrom(ancestorRoot, window, cellW, cellH);
        auto ra = rects.find(cd->children[idx].id);
        auto rb = rects.find(cd->children[neighborIdx].id);
        if (ra == rects.end() || rb == rects.end()) return false;

        int axisA = (axis == SplitDir::Horizontal) ? ra->second.w : ra->second.h;
        int axisB = (axis == SplitDir::Horizontal) ? rb->second.w : rb->second.h;

        int signedDelta = useTrailing ? pixelDelta : -pixelDelta;
        int newA = std::max(1, axisA + signedDelta);
        int newB = std::max(1, axisB - signedDelta);

        Node* pMut = node(n->parent);
        if (!pMut) return false;
        auto* cdMut = std::get_if<ContainerData>(&pMut->data);
        if (!cdMut) return false;
        cdMut->children[idx].stretch         = newA;
        cdMut->children[neighborIdx].stretch = newB;
        cdMut->children[idx].fixedCells      = 0;
        cdMut->children[neighborIdx].fixedCells = 0;
        dirty_ = true;
        return true;
    }
}

void LayoutTree::terminalLeavesIn(Uuid start, bool onlyActiveStack,
                                   std::vector<Uuid>& out) const
{
    std::function<void(Uuid)> walk = [&](Uuid id) {
        const Node* n = node(id);
        if (!n) return;
        if (n->kind() == NodeKind::Terminal) {
            out.push_back(id);
            return;
        }
        if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
            for (const auto& s : cd->children) walk(s.id);
        } else if (const auto* sd = std::get_if<StackData>(&n->data)) {
            if (onlyActiveStack) {
                if (!sd->activeChild.isNil()) walk(sd->activeChild);
            } else {
                for (const auto& c : sd->children) walk(c.id);
            }
        }
    };
    walk(start);
}

Uuid LayoutTree::leftmostTerminalIn(Uuid start) const
{
    const Node* n = node(start);
    if (!n) return {};
    if (std::holds_alternative<TerminalData>(n->data)) return start;
    if (const auto* cd = std::get_if<ContainerData>(&n->data)) {
        if (cd->children.empty()) return {};
        return leftmostTerminalIn(cd->children.front().id);
    }
    if (const auto* sd = std::get_if<StackData>(&n->data)) {
        if (sd->activeChild.isNil()) return {};
        return leftmostTerminalIn(sd->activeChild);
    }
    return {};
}

void LayoutTree::collapseSingletonsAbove(Uuid fromParent, Uuid stopAt)
{
    if (fromParent.isNil() || stopAt.isNil()) return;

    Uuid cur = fromParent;
    while (!cur.isNil() && cur != stopAt) {
        Node* n = node(cur);
        if (!n) break;
        auto* cd = std::get_if<ContainerData>(&n->data);
        if (!cd || cd->children.size() != 1) break;
        Uuid grand = n->parent;
        if (grand.isNil()) break;
        Uuid onlyChild = cd->children.front().id;
        removeChild(cur, onlyChild);
        if (!replaceChild(grand, cur, ChildSlot{onlyChild, 1})) {
            spdlog::warn("LayoutTree::collapseSingletonsAbove: replaceChild failed at {}",
                         cur.toString());
            break;
        }
        destroyNode(cur);
        cur = grand;
    }
}
