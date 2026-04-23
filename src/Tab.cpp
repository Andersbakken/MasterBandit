#include "Tab.h"

#include "LayoutTree.h"
#include "Terminal.h"
#include "script/ScriptEngine.h"

// Thin forwarders to Script::Engine::*InSubtree methods. No logic here —
// the handle exists purely for call-site ergonomics. See Tab.h.

namespace {
const std::string& kEmpty() { static const std::string s; return s; }
} // namespace

Tab Tab::newSubtree(Script::Engine* engine)
{
    if (!engine) return {};
    return Tab{engine, engine->createTabSubtree()};
}

const std::string& Tab::title() const
{
    return valid() ? eng_->tabTitle(subtreeRoot_) : kEmpty();
}

void Tab::setTitle(const std::string& s)
{
    if (valid()) eng_->setTabTitle(subtreeRoot_, s);
}

const std::string& Tab::icon() const
{
    return valid() ? eng_->tabIcon(subtreeRoot_) : kEmpty();
}

void Tab::setIcon(const std::string& s)
{
    if (valid()) eng_->setTabIcon(subtreeRoot_, s);
}

Uuid Tab::createPane()                              { return valid() ? eng_->createPaneInTab(subtreeRoot_) : Uuid{}; }
Uuid Tab::allocatePaneNode()                        { return valid() ? eng_->allocatePaneNode() : Uuid{}; }

bool Tab::splitByNodeId(Uuid existing, SplitDir dir, Uuid newChild, bool newIsFirst)
{
    return valid() && eng_->splitByNodeId(existing, dir, newChild, newIsFirst);
}

Terminal* Tab::insertTerminal(Uuid nodeId, std::unique_ptr<Terminal> t)
{
    if (!valid() || !t || nodeId.isNil()) return nullptr;
    t->setNodeId(nodeId);
    return eng_->insertTerminal(nodeId, std::move(t));
}

bool Tab::removeNodeSubtree(Uuid nodeId)
{
    if (!valid() || nodeId == subtreeRoot_) return false;
    return eng_->removeNodeSubtree(subtreeRoot_, nodeId);
}

Terminal* Tab::pane(Uuid nodeId)                    { return valid() ? eng_->paneInSubtree(subtreeRoot_, nodeId) : nullptr; }
bool      Tab::hasPaneSlot(Uuid nodeId) const       { return valid() && eng_->hasPaneSlotInSubtree(subtreeRoot_, nodeId); }
std::vector<Terminal*> Tab::panes() const           { return valid() ? eng_->panesInSubtree(subtreeRoot_) : std::vector<Terminal*>{}; }
std::vector<Terminal*> Tab::activePanes() const     { return valid() ? eng_->activePanesInSubtree(subtreeRoot_) : std::vector<Terminal*>{}; }
PaneRect  Tab::nodeRect(Uuid nodeId) const          { return valid() ? eng_->nodeRectInSubtree(subtreeRoot_, nodeId) : PaneRect{}; }
Uuid      Tab::paneAtPixel(int px, int py) const    { return valid() ? eng_->paneAtPixelInSubtree(subtreeRoot_, px, py) : Uuid{}; }

Uuid      Tab::focusedPaneId() const                { return valid() ? eng_->focusedPaneInSubtree(subtreeRoot_) : Uuid{}; }
void      Tab::setFocusedPane(Uuid nodeId)          { if (valid()) eng_->setFocusedTerminalNodeId(nodeId); }
Terminal* Tab::focusedPane()                        { return valid() ? eng_->focusedTerminalInSubtree(subtreeRoot_) : nullptr; }

void Tab::setDividerPixels(int px)                  { if (valid()) eng_->setDividerPixels(px); }
int  Tab::dividerPixels() const                     { return valid() ? eng_->dividerPixels() : 1; }

PaneRect Tab::tabBarRect(uint32_t w, uint32_t h) const
{
    return valid() ? eng_->tabBarRect(w, h) : PaneRect{};
}

void Tab::computeRects(uint32_t w, uint32_t h, int cellW, int cellH)
{
    if (valid()) eng_->computeTabRects(subtreeRoot_, w, h, cellW, cellH);
}

std::vector<PaneRect> Tab::dividerRects(int dividerPx) const
{
    return valid() ? eng_->tabDividerRects(subtreeRoot_, dividerPx) : std::vector<PaneRect>{};
}

std::vector<std::pair<Uuid, PaneRect>>
Tab::dividersWithOwnerPanes(int dividerPx) const
{
    return valid() ? eng_->tabDividersWithOwnerPanes(subtreeRoot_, dividerPx)
                   : std::vector<std::pair<Uuid, PaneRect>>{};
}

bool Tab::resizePaneEdge(Uuid nodeId, SplitDir axis, int pixelDelta)
{
    return valid() && eng_->resizeTabPaneEdge(subtreeRoot_, nodeId, axis, pixelDelta);
}
