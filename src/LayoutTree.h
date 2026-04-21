#pragma once

#include "Uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Pixel rectangle. Kept distinct from PaneRect in Terminal.h so this module
// doesn't drag in the terminal/emulator headers; a trivial adapter will
// convert at the integration boundary.
struct LayoutRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool isEmpty() const { return w <= 0 || h <= 0; }
    bool operator==(const LayoutRect&) const = default;
};

enum class NodeKind : uint8_t {
    Terminal  = 0,
    Container = 1,
    Stack     = 2,
    TabBar    = 3,
};

enum class SplitDir : uint8_t {
    Horizontal, // children laid out left to right
    Vertical,   // children laid out top to bottom
};

// Per-slot sizing hints. All cell counts are in cells along the container's
// split axis; min/max/fixed are converted to pixels at layout time using the
// cellW/cellH passed to computeRects().
struct ChildSlot {
    Uuid id;             // child node (must exist in the tree)
    int  stretch    = 1; // >=0; 0 means "don't grow"; ignored if fixedCells > 0
    int  minCells   = 0; // lower bound along split axis; 0 = no min
    int  maxCells   = 0; // upper bound; 0 = no max
    int  fixedCells = 0; // 0 = use stretch; >0 = pin to this cell count (stretch ignored)
};

struct TerminalData {
    // Empty for now. Terminal lifetime stays with the existing machinery
    // (Layout/TabManager) until the cutover; the tree only tracks identity.
};

struct ContainerData {
    SplitDir dir = SplitDir::Horizontal;
    std::vector<ChildSlot> children;
};

struct StackData {
    std::vector<ChildSlot> children; // stretch/min/max ignored (only one visible)
    Uuid activeChild;                // nil if empty stack
    bool opaque = false;             // true = navigation treats this stack as a single node
};

struct TabBarData {
    Uuid boundStack;                 // nil or dangling → renders empty
};

struct Node {
    Uuid id;
    Uuid parent; // nil = root (or detached)
    std::string label;
    std::variant<TerminalData, ContainerData, StackData, TabBarData> data;

    NodeKind kind() const { return static_cast<NodeKind>(data.index()); }
};

class LayoutTree {
public:
    // --- Node creation (returns the new node's UUID) ---
    Uuid createTerminal();
    Uuid createContainer(SplitDir dir);
    Uuid createStack();
    Uuid createTabBar();

    // --- Root management ---
    // Setting a root whose parent is not nil is rejected. The previous root
    // (if any) is NOT destroyed — caller is responsible.
    bool setRoot(Uuid id);
    Uuid root() const { return root_; }

    // --- Child management ---
    // appendChild: slot.id must exist, not already have a parent, and parent
    // must be a Container or Stack. Returns false on any violation.
    // For Stack, the first appended child becomes the active child unless
    // setActiveChild has already been called.
    bool appendChild(Uuid parent, ChildSlot slot);

    // Removes `child` from `parent`'s child list. Does NOT destroy the child
    // node — it becomes orphaned (parent = nil). If `child` was a Stack's
    // active child, the stack's activeChild clears.
    bool removeChild(Uuid parent, Uuid child);

    // Replace `oldChild` in `parent`'s child list with `newSlot` (same
    // position). `newSlot.id` must identify an orphan node (parent nil).
    // The `oldChild` becomes an orphan. For a Stack whose activeChild was
    // `oldChild`, the activeChild is retargeted to `newSlot.id`.
    // Returns false on any kind/linkage violation.
    bool replaceChild(Uuid parent, Uuid oldChild, ChildSlot newSlot);

    bool setActiveChild(Uuid stack, Uuid child);
    bool setTabBarStack(Uuid tabBar, Uuid stack); // stack must be kind Stack
    void setLabel(Uuid node, std::string label);

    // Destroy a node and, recursively, every descendant referenced through
    // Container/Stack child lists. Also clears root_ if root is destroyed.
    // TabBar bindings pointing at a destroyed Stack go dangling (not erased);
    // computeRects does not chase them, so they simply render as "empty bar."
    void destroyNode(Uuid id);

    // --- Lookup ---
    Node*       node(Uuid id);
    const Node* node(Uuid id) const;

    // --- Layout ---
    // Compute rects for the whole tree. Nodes not present in the returned map
    // are "not visible this frame" — that includes non-active Stack children
    // and container children that clipped to zero area.
    std::unordered_map<Uuid, LayoutRect, UuidHash> computeRects(
        LayoutRect window, int cellW, int cellH) const;

private:
    std::unordered_map<Uuid, std::unique_ptr<Node>, UuidHash> nodes_;
    Uuid root_;
};
