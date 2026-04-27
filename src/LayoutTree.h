#pragma once

#include "Rect.h"
#include "Uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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
    // Empty: Terminal lifetime lives in Script::Engine's terminals_ map
    // (keyed by the tree Uuid). The tree only tracks identity / topology.
};

struct ContainerData {
    SplitDir dir = SplitDir::Horizontal;
    std::vector<ChildSlot> children;
};

struct StackData {
    std::vector<ChildSlot> children; // stretch/min/max ignored (only one visible)
    Uuid activeChild;                // nil if empty stack
    bool opaque = false;             // true = navigation treats this stack as a single node
    // When non-nil, layout routes the Stack's entire rect to this node
    // (which must live in the Stack's subtree) instead of activeChild.
    // All other children are skipped that frame. JS toggles this via
    // LayoutTree::setStackZoom; destroyNode clears any Stack pointing at
    // a destroyed node. This replaces the engine-level zoomedNodeId_ policy.
    Uuid zoomTarget;
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

    // Move `child` within `parent`'s child list by `delta` positions. delta=+1
    // swaps with the next sibling, delta=-1 with the previous. Out-of-bounds
    // moves return false (caller treats as no-op). Wraparound is NOT performed
    // (use rotateChildren). Marks dirty on success. Stack `activeChild` stores
    // a Uuid, so reordering preserves focus on the moved entry automatically.
    bool moveChild(Uuid parent, Uuid child, int delta);

    // Circular shift all children of `parent` by `delta` positions. Positive
    // `delta` advances each child to a higher index; the last wraps to first.
    // Negative reverses. Returns false if parent invalid, not Container/Stack,
    // or has fewer than 2 children. Marks dirty on success.
    bool rotateChildren(Uuid parent, int delta);

    // Swap two leaves' positions across the tree, including the cross-parent
    // case. Slot weights (stretch/min/max/fixed) stay with the position so
    // the visual layout doesn't change — only the leaves move. Updates each
    // node's parent pointer and any Stack activeChild Uuids that referenced
    // a moved leaf. Refuses nil ids, equal ids, missing nodes, detached
    // nodes (no parent), or pairs where one is an ancestor of the other.
    // Marks dirty on success. Used to express "rotate / swap panes across
    // arbitrary tree shapes" in terms of a single primitive.
    bool swapLeaves(Uuid a, Uuid b);

    // Zoom override on a Stack. `target == nil` clears. Otherwise:
    //   - `stack` must be an existing Stack node.
    //   - `target` must live in `stack`'s subtree (reflexive disallowed).
    // Returns false on any violation; true (and silently clears) when
    // `target` is nil.
    bool setStackZoom(Uuid stack, Uuid target);

    // Per-slot sizing knobs (mb.layout.setSlot{Stretch,Fixed,Min,Max}).
    // `parent` must be a Container or Stack that contains `child` directly.
    // fixedCells == 0 disables fixed mode for that slot; otherwise stretch is
    // ignored in that axis. Returns false on any validation failure.
    bool setSlotStretch(Uuid parent, Uuid child, int stretch);
    bool setSlotMinCells(Uuid parent, Uuid child, int minCells);
    bool setSlotMaxCells(Uuid parent, Uuid child, int maxCells);
    bool setSlotFixedCells(Uuid parent, Uuid child, int fixedCells);

    // Destroy a node and, recursively, every descendant referenced through
    // Container/Stack child lists. Also clears root_ if root is destroyed.
    // TabBar bindings pointing at a destroyed Stack go dangling (not erased);
    // computeRects does not chase them, so they simply render as "empty bar."
    void destroyNode(Uuid id);

    // --- Lookup ---
    Node*       node(Uuid id);
    const Node* node(Uuid id) const;

    // Walk `descendant`'s parent chain; return true if any ancestor equals
    // `ancestor`. `ancestor == descendant` is also true (reflexive). Either
    // nil returns false.
    bool contains(Uuid ancestor, Uuid descendant) const;

    // Walk `start`'s parent chain (starting at `start` itself); return the
    // first ancestor whose kind() == `kind`. Returns nil if none.
    Uuid nearestAncestorOfKind(Uuid start, NodeKind kind) const;

    // --- Layout ---
    // Compute rects for the whole tree starting from root(). Nodes not in the
    // returned map are "not visible this frame" — non-active Stack children
    // and container children that clipped to zero area. Returns empty when
    // root is nil.
    std::unordered_map<Uuid, Rect, UuidHash> computeRects(
        Rect window, int cellW, int cellH) const;

    // Same as computeRects but rooted at an arbitrary node in the tree.
    // Used by Layout to lay out its own subtree inside a shared tree that
    // doesn't (yet) have the subtree hooked up under the tree root.
    std::unordered_map<Uuid, Rect, UuidHash> computeRectsFrom(
        Uuid start, Rect window, int cellW, int cellH) const;

    // Collect divider rects between visible siblings inside `start`'s subtree,
    // given a `rects` map produced by computeRects/computeRectsFrom. Each
    // divider is paired with the Uuid of its "first neighbour" — the sibling
    // on its left (horizontal split) or above (vertical split). Callers that
    // want a paneId resolve the Uuid to whatever integer identity they track.
    // Stack nodes follow only their activeChild; a Stack's zoomTarget (once
    // wired up) naturally bypasses non-zoomed Containers because their
    // children aren't in `rects`.
    void dividersIn(Uuid start, int dividerPixels,
                    const std::unordered_map<Uuid, Rect, UuidHash>& rects,
                    std::vector<std::pair<Uuid, Rect>>& out) const;

    // Starting at `fromParent`, walk up the parent chain collapsing each
    // Container that has exactly one child: the only child is promoted into
    // the grandparent's slot, and the singleton Container is destroyed.
    // Stops when it reaches `stopAt` (exclusive) or a node with 0/>=2
    // children. No-op if either argument is nil or `stopAt` is not reachable.
    void collapseSingletonsAbove(Uuid fromParent, Uuid stopAt);

    // Wrap `existingChild` in a fresh Container of direction `dir`, and
    // place `newChild` as a sibling inside that wrapper. The wrapper
    // inherits `existingChild`'s slot stretch/min/max/fixed, so the parent's
    // layout is preserved across the split. `existingChild` must have a
    // parent (Container); `newChild` must be orphaned. Returns the wrapper
    // Uuid on success, nil on any validation failure.
    Uuid splitByWrapping(Uuid existingChild, SplitDir dir,
                          Uuid newChild, bool newIsFirst);

    // Walk up from `target` until we find a Container parent whose SplitDir
    // matches `axis`. Grow `target`'s side by `pixelDelta` and shrink its
    // neighbour (leading or trailing, preferring trailing). Uses the current
    // rects (fetched via computeRectsFrom(ancestorRoot, window, cellW, cellH))
    // to translate pixels to stretch weights. Returns false if no matching
    // ancestor exists or if `ancestorRoot` is nil. `window` is the pixel box
    // the subtree lays out into.
    bool resizeEdgeAlongAxis(Uuid target, SplitDir axis, int pixelDelta,
                              Uuid ancestorRoot, Rect window,
                              int cellW, int cellH);

    // Collect every Terminal-kind leaf Uuid in `start`'s subtree. If
    // `onlyActiveStack` is true, Stack recursion follows `activeChild` only
    // (matches the "currently visible" semantics).
    void terminalLeavesIn(Uuid start, bool onlyActiveStack,
                          std::vector<Uuid>& out) const;

    // Return the leftmost Terminal's Uuid inside `start`'s subtree — useful
    // for naming dividers by their "first neighbour" paneId.
    Uuid leftmostTerminalIn(Uuid start) const;

    // Dirty flag — set by every structural mutation (appendChild, removeChild,
    // replaceChild, setActiveChild, setStackZoom, setSlot*, setRoot,
    // destroyNode). Consumers (PlatformDawn::runLayoutIfDirty) read via
    // takeDirty() to recompute rects + cascade TIOCSWINSZ exactly once per
    // frame, regardless of how many mutations accumulated. External callers
    // (e.g. framebuffer resize) can manually markDirty().
    void markDirty() { dirty_ = true; ++revision_; }
    bool takeDirty() { bool d = dirty_; dirty_ = false; return d; }
    bool isDirty() const { return dirty_; }

    // Monotonic revision counter — bumped on every markDirty. Unlike the
    // dirty flag, this is never consumed. Callers that cache derived
    // information (e.g. the root-level computeRects map in Engine) stamp
    // their cache with the revision at build time and invalidate when it
    // diverges. Wraps silently at 2^64 mutations; don't hold your breath.
    uint64_t revision() const { return revision_; }

private:
    std::unordered_map<Uuid, std::unique_ptr<Node>, UuidHash> nodes_;
    Uuid root_;
    bool dirty_ = true; // initial: first frame must compute
    uint64_t revision_ = 1; // caches track this; starts at 1 so 0 = "no cache"
};
