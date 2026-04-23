#pragma once

// Thin ergonomic handle for a tab. All real logic lives in Script::Engine
// as `*InSubtree` methods; Tab just holds {Engine*, subtreeRoot Uuid} and
// forwards. No .cpp — everything here is inline and one-line.
//
// Kept as a value type mainly for call-site ergonomics (`tab.panes()`
// vs. `engine->panesInSubtree(tab.subtreeRoot())`). Dissolving it fully
// would mean sweeping ~164 call sites; see REFACTOR_CONTINUED.md.

#include "LayoutTree.h"
#include "Rect.h"
#include "Uuid.h"

#include <memory>
#include <string>
#include <vector>

class Terminal;
namespace Script { class Engine; }

using PaneRect = Rect;

// Non-owning handle for a tab. A tab's identity is its subtreeRoot Uuid in
// the shared LayoutTree (a direct child of Engine::layoutRootStack_).
struct Tab {
    Tab() = default;
    Tab(Script::Engine* eng, Uuid subtreeRoot) : eng_(eng), subtreeRoot_(subtreeRoot) {}

    // Create a fresh tab subtree on `engine`'s shared LayoutTree.
    static Tab newSubtree(Script::Engine* engine);

    bool valid() const { return eng_ && !subtreeRoot_.isNil(); }
    explicit operator bool() const { return valid(); }

    Uuid subtreeRoot() const { return subtreeRoot_; }
    Script::Engine* engine() const { return eng_; }

    // Each method below forwards to an Engine::*InSubtree variant (or
    // direct Engine method where scoping is implicit). Definitions live
    // in Tab.cpp-free inline body via ScriptEngine.h.
    const std::string& title() const;
    void setTitle(const std::string& s);
    const std::string& icon() const;
    void setIcon(const std::string& s);

    Uuid createPane();
    Uuid allocatePaneNode();
    bool splitByNodeId(Uuid existingChildNodeId, SplitDir dir,
                       Uuid newChildNodeId, bool newIsFirst = false);
    Terminal* insertTerminal(Uuid nodeId, std::unique_ptr<Terminal> t);
    bool removeNodeSubtree(Uuid nodeId);

    Terminal* pane(Uuid nodeId);
    bool hasPaneSlot(Uuid nodeId) const;
    std::vector<Terminal*> panes() const;
    std::vector<Terminal*> activePanes() const;
    PaneRect nodeRect(Uuid nodeId) const;
    Uuid paneAtPixel(int px, int py) const;

    Uuid focusedPaneId() const;
    void setFocusedPane(Uuid nodeId);
    Terminal* focusedPane();

    void setDividerPixels(int px);
    int dividerPixels() const;
    PaneRect tabBarRect(uint32_t windowW, uint32_t windowH) const;

    void computeRects(uint32_t windowW, uint32_t windowH, int cellW, int cellH);
    std::vector<PaneRect> dividerRects(int dividerPixels) const;
    std::vector<std::pair<Uuid, PaneRect>> dividersWithOwnerPanes(int dividerPixels) const;
    bool resizePaneEdge(Uuid nodeId, SplitDir axis, int pixelDelta);

private:
    Script::Engine* eng_ = nullptr;
    Uuid subtreeRoot_;
};
