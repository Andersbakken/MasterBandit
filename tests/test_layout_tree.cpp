// Unit tests for LayoutTree's computeRects — pinning down the stretch/fixed
// sizing, overflow clipping, Stack visibility rule, and nested recursion.
//
// Tests use cellW = cellH = 1 unless otherwise noted so "cells" math reads
// as pixels, which keeps assertion numbers obvious.

#include <doctest/doctest.h>

#include "LayoutTree.h"

namespace {

// Convenience: read a rect if present, else {} so tests can assert on absence.
LayoutRect rectOf(const std::unordered_map<Uuid, LayoutRect, UuidHash>& map, Uuid id)
{
    auto it = map.find(id);
    return it == map.end() ? LayoutRect{} : it->second;
}

bool isPresent(const std::unordered_map<Uuid, LayoutRect, UuidHash>& map, Uuid id)
{
    return map.count(id) > 0;
}

} // namespace

TEST_CASE("LayoutTree: empty tree produces empty map")
{
    LayoutTree t;
    auto m = t.computeRects({0, 0, 800, 600}, 1, 1);
    CHECK(m.empty());
}

TEST_CASE("LayoutTree: single Terminal as root fills the window")
{
    LayoutTree t;
    Uuid term = t.createTerminal();
    REQUIRE(t.setRoot(term));

    auto m = t.computeRects({0, 0, 800, 600}, 1, 1);
    CHECK(m.size() == 1);
    CHECK(rectOf(m, term) == LayoutRect{0, 0, 800, 600});
}

TEST_CASE("LayoutTree: horizontal container with equal stretch splits evenly")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a, 1, 0, 0, 0}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1, 0, 0, 0}));
    REQUIRE(t.appendChild(root, ChildSlot{c, 1, 0, 0, 0}));

    auto m = t.computeRects({0, 0, 300, 100}, 1, 1);
    CHECK(m.size() == 4);
    CHECK(rectOf(m, a) == LayoutRect{  0, 0, 100, 100});
    CHECK(rectOf(m, b) == LayoutRect{100, 0, 100, 100});
    CHECK(rectOf(m, c) == LayoutRect{200, 0, 100, 100});
}

TEST_CASE("LayoutTree: stretch factors distribute remaining space proportionally")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 2}));
    REQUIRE(t.appendChild(root, ChildSlot{c, 1}));

    auto m = t.computeRects({0, 0, 400, 100}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{  0, 0, 100, 100});
    CHECK(rectOf(m, b) == LayoutRect{100, 0, 200, 100});
    CHECK(rectOf(m, c) == LayoutRect{300, 0, 100, 100});
}

TEST_CASE("LayoutTree: fixed-size child pins its dimension, rest stretches")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    ChildSlot fixed{a};
    fixed.fixedCells = 50;  // pin to 50 cells
    REQUIRE(t.appendChild(root, fixed));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1}));

    auto m = t.computeRects({0, 0, 200, 50}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0,  50, 50});
    CHECK(rectOf(m, b) == LayoutRect{50, 0, 150, 50});
}

TEST_CASE("LayoutTree: vertical container stacks children top to bottom")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Vertical);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1}));

    auto m = t.computeRects({10, 20, 400, 200}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{10,  20, 400, 100});
    CHECK(rectOf(m, b) == LayoutRect{10, 120, 400, 100});
}

TEST_CASE("LayoutTree: min cells clamps a child above its stretch share")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    // Equal stretch would give each 50; min on `a` pulls it up to 80.
    REQUIRE(t.appendChild(root, ChildSlot{a, 1, 80, 0, 0}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1, 0,  0, 0}));

    auto m = t.computeRects({0, 0, 100, 50}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0, 80, 50});
    CHECK(rectOf(m, b) == LayoutRect{80, 0, 20, 50});
}

TEST_CASE("LayoutTree: max cells caps a child below its stretch share")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    // Equal stretch would give each 50; max on `a` caps it at 20.
    // The leftover gets redistributed to the last flexible child.
    REQUIRE(t.appendChild(root, ChildSlot{a, 1, 0, 20, 0}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1, 0,  0, 0}));

    auto m = t.computeRects({0, 0, 100, 50}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0, 20, 50});
    CHECK(rectOf(m, b) == LayoutRect{20, 0, 80, 50});
}

TEST_CASE("LayoutTree: zero-stretch child collapses to min; nonzero-stretch takes the rest")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a, 0, 30, 0, 0})); // fixed-ish via min
    REQUIRE(t.appendChild(root, ChildSlot{b, 1,  0, 0, 0}));

    auto m = t.computeRects({0, 0, 200, 50}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0,  30, 50});
    CHECK(rectOf(m, b) == LayoutRect{30, 0, 170, 50});
}

TEST_CASE("LayoutTree: overflow clips trailing children to zero (and they drop from the map)")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(root));
    // Each child demands min 50 cells; window is only 80px wide.
    REQUIRE(t.appendChild(root, ChildSlot{a, 1, 50}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1, 50}));
    REQUIRE(t.appendChild(root, ChildSlot{c, 1, 50}));

    auto m = t.computeRects({0, 0, 80, 50}, 1, 1);
    // `a` keeps its full 50. `b` would be 50 but that overflows, so it gets
    // shrunk to the remaining 30 (shrink-below-min after clipping `c`).
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0, 50, 50});
    CHECK(rectOf(m, b) == LayoutRect{50, 0, 30, 50});
    CHECK_FALSE(isPresent(m, c));
}

TEST_CASE("LayoutTree: Stack shows only its active child, others are absent")
{
    LayoutTree t;
    Uuid stack = t.createStack();
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(stack));
    REQUIRE(t.appendChild(stack, ChildSlot{a}));
    REQUIRE(t.appendChild(stack, ChildSlot{b}));
    REQUIRE(t.appendChild(stack, ChildSlot{c}));

    // First-appended child defaulted to active.
    auto m = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m, stack) == LayoutRect{0, 0, 200, 100});
    CHECK(rectOf(m, a) == LayoutRect{0, 0, 200, 100});
    CHECK_FALSE(isPresent(m, b));
    CHECK_FALSE(isPresent(m, c));

    // Switch active and re-layout.
    REQUIRE(t.setActiveChild(stack, c));
    m = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m, c) == LayoutRect{0, 0, 200, 100});
    CHECK_FALSE(isPresent(m, a));
    CHECK_FALSE(isPresent(m, b));
}

TEST_CASE("LayoutTree: empty Stack just emits its own rect")
{
    LayoutTree t;
    Uuid stack = t.createStack();
    REQUIRE(t.setRoot(stack));

    auto m = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(m.size() == 1);
    CHECK(rectOf(m, stack) == LayoutRect{0, 0, 200, 100});
}

TEST_CASE("LayoutTree: nested containers — splitv inside splith")
{
    //   root (horizontal)
    //   ├── left terminal                 (stretch=1)
    //   └── inner (vertical, stretch=1)
    //       ├── top terminal              (stretch=1)
    //       └── bot terminal              (stretch=1)
    LayoutTree t;
    Uuid root  = t.createContainer(SplitDir::Horizontal);
    Uuid left  = t.createTerminal();
    Uuid inner = t.createContainer(SplitDir::Vertical);
    Uuid top   = t.createTerminal();
    Uuid bot   = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root,  ChildSlot{left,  1}));
    REQUIRE(t.appendChild(root,  ChildSlot{inner, 1}));
    REQUIRE(t.appendChild(inner, ChildSlot{top,   1}));
    REQUIRE(t.appendChild(inner, ChildSlot{bot,   1}));

    auto m = t.computeRects({0, 0, 400, 200}, 1, 1);
    CHECK(rectOf(m, root)  == LayoutRect{  0,   0, 400, 200});
    CHECK(rectOf(m, left)  == LayoutRect{  0,   0, 200, 200});
    CHECK(rectOf(m, inner) == LayoutRect{200,   0, 200, 200});
    CHECK(rectOf(m, top)   == LayoutRect{200,   0, 200, 100});
    CHECK(rectOf(m, bot)   == LayoutRect{200, 100, 200, 100});
}

TEST_CASE("LayoutTree: TabBar treated as a plain leaf occupying its slot")
{
    // Classic shape: root splitv with a fixed-height TabBar on top and a
    // Stack (the workspace area) below.
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Vertical);
    Uuid bar  = t.createTabBar();
    Uuid stk  = t.createStack();
    Uuid ws1  = t.createTerminal();
    REQUIRE(t.setRoot(root));
    ChildSlot barSlot{bar, 0, 0, 0, 20};  // 20 cells tall, no stretch
    REQUIRE(t.appendChild(root, barSlot));
    REQUIRE(t.appendChild(root, ChildSlot{stk, 1}));
    REQUIRE(t.appendChild(stk,  ChildSlot{ws1}));
    REQUIRE(t.setTabBarStack(bar, stk));

    auto m = t.computeRects({0, 0, 400, 200}, 1, 1);
    CHECK(rectOf(m, bar) == LayoutRect{0,  0, 400,  20});
    CHECK(rectOf(m, stk) == LayoutRect{0, 20, 400, 180});
    CHECK(rectOf(m, ws1) == LayoutRect{0, 20, 400, 180});
}

TEST_CASE("LayoutTree: cell size > 1 scales the sizing math")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(root));
    // `a` is pinned to 10 cells; cellW = 8 px → 80 px.
    ChildSlot pinned{a};
    pinned.fixedCells = 10;
    REQUIRE(t.appendChild(root, pinned));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1}));

    auto m = t.computeRects({0, 0, 200, 40}, 8, 16);
    CHECK(rectOf(m, a) == LayoutRect{ 0, 0,  80, 40});
    CHECK(rectOf(m, b) == LayoutRect{80, 0, 120, 40});
}

TEST_CASE("LayoutTree: appendChild rejects second parent for the same node")
{
    LayoutTree t;
    Uuid p1 = t.createContainer(SplitDir::Horizontal);
    Uuid p2 = t.createContainer(SplitDir::Horizontal);
    Uuid c  = t.createTerminal();
    CHECK(t.appendChild(p1, ChildSlot{c}));
    CHECK_FALSE(t.appendChild(p2, ChildSlot{c}));
}

TEST_CASE("LayoutTree: setTabBarStack rejects a non-Stack target")
{
    LayoutTree t;
    Uuid bar  = t.createTabBar();
    Uuid term = t.createTerminal();
    CHECK_FALSE(t.setTabBarStack(bar, term));
    // nil clears the binding and is accepted.
    CHECK(t.setTabBarStack(bar, Uuid{}));
}

TEST_CASE("LayoutTree: destroyNode tears down the subtree and clears dangling structural refs")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid inner = t.createContainer(SplitDir::Vertical);
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a}));
    REQUIRE(t.appendChild(root, ChildSlot{inner}));
    REQUIRE(t.appendChild(inner, ChildSlot{b}));
    REQUIRE(t.appendChild(inner, ChildSlot{c}));

    t.destroyNode(inner);

    CHECK(t.node(inner) == nullptr);
    CHECK(t.node(b)     == nullptr);
    CHECK(t.node(c)     == nullptr);
    // `a` survives; root now has only one child (`a`).
    REQUIRE(t.node(root) != nullptr);
    auto m = t.computeRects({0, 0, 100, 50}, 1, 1);
    CHECK(rectOf(m, a) == LayoutRect{0, 0, 100, 50});
    CHECK_FALSE(isPresent(m, inner));
}

TEST_CASE("LayoutTree: destroyNode on root clears root_ and leaves tree queryable")
{
    LayoutTree t;
    Uuid root = t.createTerminal();
    REQUIRE(t.setRoot(root));
    t.destroyNode(root);
    CHECK(t.root().isNil());

    auto m = t.computeRects({0, 0, 100, 100}, 1, 1);
    CHECK(m.empty());
}

TEST_CASE("LayoutTree: removing a stack's active child promotes the new front to active")
{
    LayoutTree t;
    Uuid stk = t.createStack();
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(stk));
    REQUIRE(t.appendChild(stk, ChildSlot{a}));
    REQUIRE(t.appendChild(stk, ChildSlot{b}));
    // `a` was auto-activated on first append.
    REQUIRE(t.removeChild(stk, a));

    auto m = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m, b) == LayoutRect{0, 0, 200, 100});
}

TEST_CASE("LayoutTree: contains walks parent chain")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid inner = t.createContainer(SplitDir::Vertical);
    Uuid leaf = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{inner}));
    REQUIRE(t.appendChild(inner, ChildSlot{leaf}));

    CHECK(t.contains(root, leaf));
    CHECK(t.contains(root, inner));
    CHECK(t.contains(root, root)); // reflexive
    CHECK(t.contains(inner, leaf));
    CHECK_FALSE(t.contains(leaf, root)); // reversed
    CHECK_FALSE(t.contains(root, Uuid{}));
    CHECK_FALSE(t.contains(Uuid{}, leaf));
}

TEST_CASE("LayoutTree: nearestAncestorOfKind finds first matching ancestor")
{
    LayoutTree t;
    Uuid stk = t.createStack();
    Uuid inner = t.createContainer(SplitDir::Horizontal);
    Uuid leaf = t.createTerminal();
    REQUIRE(t.setRoot(stk));
    REQUIRE(t.appendChild(stk, ChildSlot{inner}));
    REQUIRE(t.appendChild(inner, ChildSlot{leaf}));

    // `leaf` itself is the Terminal match (reflexive).
    CHECK(t.nearestAncestorOfKind(leaf, NodeKind::Terminal) == leaf);
    // `inner` is the nearest Container.
    CHECK(t.nearestAncestorOfKind(leaf, NodeKind::Container) == inner);
    // `stk` is the nearest Stack (skipping past inner).
    CHECK(t.nearestAncestorOfKind(leaf, NodeKind::Stack) == stk);
    // No TabBar anywhere: nil.
    CHECK(t.nearestAncestorOfKind(leaf, NodeKind::TabBar).isNil());
}

TEST_CASE("LayoutTree: dividersIn emits rects between visible siblings")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{a, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{b, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{c, 1}));

    auto rects = t.computeRects({0, 0, 300, 100}, 1, 1);
    std::vector<std::pair<Uuid, LayoutRect>> divs;
    t.dividersIn(root, 2, rects, divs);

    REQUIRE(divs.size() == 2);
    // First divider between a and b. Owner = a.
    CHECK(divs[0].first == a);
    CHECK(divs[0].second == LayoutRect{100, 0, 2, 100});
    // Second divider between b and c. Owner = b.
    CHECK(divs[1].first == b);
    CHECK(divs[1].second == LayoutRect{200, 0, 2, 100});
}

TEST_CASE("LayoutTree: dividersIn follows only active Stack child")
{
    LayoutTree t;
    Uuid stk = t.createStack();
    Uuid visible = t.createContainer(SplitDir::Horizontal);
    Uuid hidden  = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    Uuid c = t.createTerminal();
    Uuid d = t.createTerminal();
    REQUIRE(t.setRoot(stk));
    REQUIRE(t.appendChild(stk, ChildSlot{visible}));
    REQUIRE(t.appendChild(stk, ChildSlot{hidden}));
    REQUIRE(t.appendChild(visible, ChildSlot{a, 1}));
    REQUIRE(t.appendChild(visible, ChildSlot{b, 1}));
    REQUIRE(t.appendChild(hidden,  ChildSlot{c, 1}));
    REQUIRE(t.appendChild(hidden,  ChildSlot{d, 1}));
    // `visible` was auto-activated by first appendChild.

    auto rects = t.computeRects({0, 0, 200, 50}, 1, 1);
    std::vector<std::pair<Uuid, LayoutRect>> divs;
    t.dividersIn(stk, 1, rects, divs);

    // Only the visible Container's a/b boundary should produce a divider.
    REQUIRE(divs.size() == 1);
    CHECK(divs[0].first == a);
}

TEST_CASE("LayoutTree: revision counter is monotonic across mutations")
{
    LayoutTree t;
    uint64_t r0 = t.revision();
    // Create ops don't mutate structure, no bump.
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    CHECK(t.revision() == r0);

    REQUIRE(t.setRoot(root));
    uint64_t r1 = t.revision();
    CHECK(r1 > r0);

    REQUIRE(t.appendChild(root, ChildSlot{a}));
    uint64_t r2 = t.revision();
    CHECK(r2 > r1);

    // Read-only ops don't bump.
    (void)t.computeRects({0, 0, 100, 50}, 1, 1);
    (void)t.contains(root, a);
    CHECK(t.revision() == r2);

    // Consuming takeDirty() does NOT reset the revision.
    (void)t.takeDirty();
    CHECK(t.revision() == r2);

    REQUIRE(t.setSlotStretch(root, a, 3));
    CHECK(t.revision() > r2);
}

TEST_CASE("LayoutTree: dirty flag tracks mutations")
{
    LayoutTree t;
    // Fresh tree starts dirty (first-frame compute).
    CHECK(t.isDirty());
    CHECK(t.takeDirty());
    CHECK_FALSE(t.isDirty());

    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid a    = t.createTerminal();
    // createContainer/createTerminal are allocation-only; no structural
    // change until setRoot/appendChild. They do NOT dirty the tree.
    CHECK_FALSE(t.isDirty());

    REQUIRE(t.setRoot(root));
    CHECK(t.isDirty());
    (void)t.takeDirty();

    REQUIRE(t.appendChild(root, ChildSlot{a, 1}));
    CHECK(t.isDirty());
    (void)t.takeDirty();

    REQUIRE(t.setSlotStretch(root, a, 2));
    CHECK(t.isDirty());
    (void)t.takeDirty();

    // Read-only ops should NOT dirty.
    (void)t.computeRects({0, 0, 100, 50}, 1, 1);
    (void)t.contains(root, a);
    CHECK_FALSE(t.isDirty());

    t.destroyNode(a);
    CHECK(t.isDirty());
}

TEST_CASE("LayoutTree: Stack zoomTarget routes the Stack's rect to the target")
{
    LayoutTree t;
    Uuid stk  = t.createStack();
    Uuid cont = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    Uuid b = t.createTerminal();
    REQUIRE(t.setRoot(stk));
    REQUIRE(t.appendChild(stk,  ChildSlot{cont}));
    REQUIRE(t.appendChild(cont, ChildSlot{a, 1}));
    REQUIRE(t.appendChild(cont, ChildSlot{b, 1}));

    // Sanity: without zoom both siblings share the rect.
    auto m0 = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m0, a) == LayoutRect{  0, 0, 100, 100});
    CHECK(rectOf(m0, b) == LayoutRect{100, 0, 100, 100});

    // Zoom `a` — the Stack's entire rect goes to a, b is missing.
    REQUIRE(t.setStackZoom(stk, a));
    auto m1 = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m1, a) == LayoutRect{0, 0, 200, 100});
    CHECK(m1.find(b) == m1.end());

    // Clear: back to the split.
    REQUIRE(t.setStackZoom(stk, Uuid{}));
    auto m2 = t.computeRects({0, 0, 200, 100}, 1, 1);
    CHECK(rectOf(m2, b) == LayoutRect{100, 0, 100, 100});
}

TEST_CASE("LayoutTree: setStackZoom rejects targets outside the Stack's subtree")
{
    LayoutTree t;
    Uuid stk   = t.createStack();
    Uuid inner = t.createTerminal();
    Uuid orphan = t.createTerminal(); // not under stk
    REQUIRE(t.appendChild(stk, ChildSlot{inner}));
    CHECK_FALSE(t.setStackZoom(stk, orphan));
    CHECK_FALSE(t.setStackZoom(stk, stk)); // reflexive disallowed
    CHECK(t.setStackZoom(stk, inner));
    CHECK(t.setStackZoom(stk, Uuid{}));     // nil always clears
}

TEST_CASE("LayoutTree: destroyNode clears zoomTarget pointing into the destroyed subtree")
{
    LayoutTree t;
    Uuid stk  = t.createStack();
    Uuid cont = t.createContainer(SplitDir::Horizontal);
    Uuid a = t.createTerminal();
    REQUIRE(t.setRoot(stk));
    REQUIRE(t.appendChild(stk,  ChildSlot{cont}));
    REQUIRE(t.appendChild(cont, ChildSlot{a, 1}));
    REQUIRE(t.setStackZoom(stk, a));

    // Destroy a — zoomTarget must clear, else zoom override would dangle.
    t.destroyNode(a);
    const Node* s = t.node(stk);
    REQUIRE(s != nullptr);
    const auto* sd = std::get_if<StackData>(&s->data);
    REQUIRE(sd != nullptr);
    CHECK(sd->zoomTarget.isNil());
}

TEST_CASE("LayoutTree: collapseSingletonsAbove folds single-child Containers")
{
    LayoutTree t;
    Uuid root  = t.createContainer(SplitDir::Horizontal);
    Uuid wrap1 = t.createContainer(SplitDir::Vertical);
    Uuid wrap2 = t.createContainer(SplitDir::Horizontal);
    Uuid leaf  = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root,  ChildSlot{wrap1, 1}));
    REQUIRE(t.appendChild(wrap1, ChildSlot{wrap2, 1}));
    REQUIRE(t.appendChild(wrap2, ChildSlot{leaf,  1}));

    // Collapse everything from wrap2 up to (but not including) root.
    t.collapseSingletonsAbove(wrap2, root);

    // wrap1, wrap2 are gone; root directly parents leaf.
    CHECK(t.node(wrap1) == nullptr);
    CHECK(t.node(wrap2) == nullptr);
    auto rects = t.computeRects({0, 0, 100, 50}, 1, 1);
    CHECK(rectOf(rects, leaf) == LayoutRect{0, 0, 100, 50});
}
