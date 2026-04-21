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
