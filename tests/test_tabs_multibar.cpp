// Multi-TabBar layout invariants. Pure LayoutTree — no Engine, no Platform.
// Verifies the structural property that the post-flip API depends on:
// independent TabBars bound to independent Stacks have independent
// activeChild state.

#include <doctest/doctest.h>
#include "LayoutTree.h"
#include "Uuid.h"

#include <unordered_set>
#include <vector>

namespace {

// Mini walker mirroring Engine::queryNodesByKind for the tree-only test.
// Implementation duplicated here to keep this test free of Engine deps.
std::vector<Uuid> walkByKind(const LayoutTree& t, NodeKind kind, Uuid start)
{
    std::vector<Uuid> out;
    if (start.isNil()) start = t.root();
    if (start.isNil()) return out;

    std::vector<Uuid> queue{start};
    while (!queue.empty()) {
        Uuid cur = queue.back();
        queue.pop_back();
        const Node* n = t.node(cur);
        if (!n) continue;
        if (n->kind() == kind) out.push_back(cur);
        std::visit([&](const auto& d) {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, ContainerData>) {
                for (const auto& s : d.children) queue.push_back(s.id);
            } else if constexpr (std::is_same_v<T, StackData>) {
                for (const auto& s : d.children) queue.push_back(s.id);
            }
        }, n->data);
    }
    return out;
}

} // namespace

TEST_CASE("multibar: two TabBars bound to two Stacks have independent activeChild")
{
    // Layout shape:
    //   Container (root, vertical)
    //   ├── TabBar (barA, bound to stackA)
    //   ├── Stack (stackA)            ← top half: tabs A0, A1
    //   │   ├── Terminal A0           (activeChild)
    //   │   └── Terminal A1
    //   ├── TabBar (barB, bound to stackB)
    //   └── Stack (stackB)            ← bottom half: tabs B0, B1
    //       ├── Terminal B0           (activeChild)
    //       └── Terminal B1
    LayoutTree t;
    Uuid root   = t.createContainer(SplitDir::Vertical);
    Uuid barA   = t.createTabBar();
    Uuid stackA = t.createStack();
    Uuid barB   = t.createTabBar();
    Uuid stackB = t.createStack();
    Uuid a0     = t.createTerminal();
    Uuid a1     = t.createTerminal();
    Uuid b0     = t.createTerminal();
    Uuid b1     = t.createTerminal();

    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{barA,   0, 0, 0, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{stackA, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{barB,   0, 0, 0, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{stackB, 1}));
    REQUIRE(t.appendChild(stackA, ChildSlot{a0}));
    REQUIRE(t.appendChild(stackA, ChildSlot{a1}));
    REQUIRE(t.appendChild(stackB, ChildSlot{b0}));
    REQUIRE(t.appendChild(stackB, ChildSlot{b1}));
    REQUIRE(t.setTabBarStack(barA, stackA));
    REQUIRE(t.setTabBarStack(barB, stackB));

    // queryNodes("TabBar") finds both bars; their boundStacks differ.
    auto bars = walkByKind(t, NodeKind::TabBar, {});
    REQUIRE(bars.size() == 2);
    std::unordered_set<Uuid, UuidHash> barSet(bars.begin(), bars.end());
    CHECK(barSet.count(barA) == 1);
    CHECK(barSet.count(barB) == 1);

    const Node* bna = t.node(barA);
    const Node* bnb = t.node(barB);
    REQUIRE(bna);
    REQUIRE(bnb);
    auto* bda = std::get_if<TabBarData>(&bna->data);
    auto* bdb = std::get_if<TabBarData>(&bnb->data);
    REQUIRE(bda);
    REQUIRE(bdb);
    CHECK(bda->boundStack == stackA);
    CHECK(bdb->boundStack == stackB);
    CHECK(bda->boundStack != bdb->boundStack);

    // Initial state: each Stack's activeChild is the first appended.
    auto* sda = std::get_if<StackData>(&t.node(stackA)->data);
    auto* sdb = std::get_if<StackData>(&t.node(stackB)->data);
    REQUIRE(sda);
    REQUIRE(sdb);
    CHECK(sda->activeChild == a0);
    CHECK(sdb->activeChild == b0);

    // Activating a tab in barA's stack must not touch barB's stack.
    REQUIRE(t.setActiveChild(stackA, a1));
    sda = std::get_if<StackData>(&t.node(stackA)->data);
    sdb = std::get_if<StackData>(&t.node(stackB)->data);
    CHECK(sda->activeChild == a1);
    CHECK(sdb->activeChild == b0);

    // …and vice versa.
    REQUIRE(t.setActiveChild(stackB, b1));
    sda = std::get_if<StackData>(&t.node(stackA)->data);
    sdb = std::get_if<StackData>(&t.node(stackB)->data);
    CHECK(sda->activeChild == a1);
    CHECK(sdb->activeChild == b1);
}

TEST_CASE("multibar: queryNodes(kind, subtreeRoot) scopes the walk")
{
    // Tree:
    //   Container (root)
    //   ├── Stack (sA) → Terminal aT
    //   └── Stack (sB) → Container (cB) → Terminal bT
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid sA = t.createStack();
    Uuid sB = t.createStack();
    Uuid cB = t.createContainer(SplitDir::Vertical);
    Uuid aT = t.createTerminal();
    Uuid bT = t.createTerminal();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{sA, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{sB, 1}));
    REQUIRE(t.appendChild(sA,   ChildSlot{aT}));
    REQUIRE(t.appendChild(sB,   ChildSlot{cB}));
    REQUIRE(t.appendChild(cB,   ChildSlot{bT}));

    auto allTerm = walkByKind(t, NodeKind::Terminal, {});
    CHECK(allTerm.size() == 2);

    auto justA = walkByKind(t, NodeKind::Terminal, sA);
    REQUIRE(justA.size() == 1);
    CHECK(justA[0] == aT);

    auto justB = walkByKind(t, NodeKind::Terminal, sB);
    REQUIRE(justB.size() == 1);
    CHECK(justB[0] == bT);

    // No TabBars in this tree.
    CHECK(walkByKind(t, NodeKind::TabBar, {}).empty());
}

TEST_CASE("multibar: setLabel + tree walk lookup by label")
{
    LayoutTree t;
    Uuid root = t.createContainer(SplitDir::Horizontal);
    Uuid sA = t.createStack();
    Uuid sB = t.createStack();
    Uuid sC = t.createStack();
    REQUIRE(t.setRoot(root));
    REQUIRE(t.appendChild(root, ChildSlot{sA, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{sB, 1}));
    REQUIRE(t.appendChild(root, ChildSlot{sC, 1}));

    t.setLabel(sA, "editor");
    t.setLabel(sB, "logs");
    // sC is unlabeled.

    // Verify labels are set on the nodes themselves.
    REQUIRE(t.node(sA));
    REQUIRE(t.node(sB));
    REQUIRE(t.node(sC));
    CHECK(t.node(sA)->label == "editor");
    CHECK(t.node(sB)->label == "logs");
    CHECK(t.node(sC)->label.empty());

    // Mini findByLabel: walk siblings, return first match.
    auto findByLabel = [&](const std::string& want) -> Uuid {
        for (Uuid u : walkByKind(t, NodeKind::Stack, {})) {
            if (t.node(u)->label == want) return u;
        }
        return {};
    };
    CHECK(findByLabel("editor") == sA);
    CHECK(findByLabel("logs") == sB);
    CHECK(findByLabel("nonexistent").isNil());
}
