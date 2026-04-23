#include "TabManager.h"

#include "Graveyard.h"
#include "InputController.h"
#include "Layout.h"
#include "PlatformUtils.h"
#include "ScriptEngine.h"
#include "Terminal.h"

#include <eventloop/EventLoop.h>
#include <eventloop/Window.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Tree-backed tab identity
//
// Each tab is a direct child of Engine::layoutRootStack_ in the shared
// LayoutTree. The tab's identity is its subtreeRoot Uuid. Icon lives in
// Script::Engine, keyed by that Uuid. Title lives on the tree node's label.
// Everything here is a thin wrapper around the tree + Engine maps.
// ---------------------------------------------------------------------------

// Identity surface delegates to Engine — single source of tree-derived truth.

Uuid TabManager::tabSubtreeRootAt(int idx) const
{
    if (!host_.scriptEngine || idx < 0) return {};
    auto roots = host_.scriptEngine->tabSubtreeRoots();
    if (idx >= static_cast<int>(roots.size())) return {};
    return roots[idx];
}

std::vector<Tab> TabManager::tabs() const
{
    std::vector<Tab> out;
    if (!host_.scriptEngine) return out;
    auto roots = host_.scriptEngine->tabSubtreeRoots();
    out.reserve(roots.size());
    for (Uuid u : roots) out.emplace_back(host_.scriptEngine, u);
    return out;
}

size_t TabManager::size() const
{
    return host_.scriptEngine ? host_.scriptEngine->tabCount() : 0;
}

int TabManager::activeTabIdx() const
{
    return host_.scriptEngine ? host_.scriptEngine->activeTabIndex() : -1;
}

std::optional<Tab> TabManager::activeTab() const
{
    int idx = activeTabIdx();
    if (idx < 0) return std::nullopt;
    return tabAt(idx);
}

std::optional<Tab> TabManager::tabAt(int idx) const
{
    Uuid sub = tabSubtreeRootAt(idx);
    if (sub.isNil()) return std::nullopt;
    return Tab{host_.scriptEngine, sub};
}

Terminal* TabManager::activeTerm()
{
    auto tab = activeTab();
    if (!tab) return nullptr;
    Terminal* pane = tab->focusedPane();
    return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
}


void TabManager::notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn)
{
    for (Tab tab : tabs()) {
        if (tab.valid()) {
            for (Terminal* panePtr : tab.panes()) fn(panePtr);
        }
    }
}


std::optional<Tab> TabManager::findTabForPane(Uuid nodeId, int* outTabIdx) const
{
    // hasPaneSlot (not pane()) so a killed-but-not-yet-removed Terminal's
    // enclosing Tab is still resolvable — the controller needs it to drive
    // tree removal in response to `terminalExited`.
    auto allTabs = tabs();
    for (int i = 0; i < static_cast<int>(allTabs.size()); ++i) {
        if (allTabs[i].valid() && allTabs[i].hasPaneSlot(nodeId)) {
            if (outTabIdx) *outTabIdx = i;
            return allTabs[i];
        }
    }
    return std::nullopt;
}


void TabManager::setActiveTabIdx(int idx)
{
    if (!host_.scriptEngine) return;
    Uuid sub = tabSubtreeRootAt(idx);
    if (sub.isNil()) return;
    host_.scriptEngine->layoutTree().setActiveChild(
        host_.scriptEngine->layoutRootStack(), sub);

    // The engine holds a single focused-terminal Uuid. If the currently
    // focused Terminal isn't inside the newly active tab, move focus onto
    // the first Terminal of that tab. Without this, closing a tab (which
    // clears focus if the focused pane was inside it) and splitting via a
    // JS action that reads `mb.layout.focusedPane()` would see null focus
    // and no-op the split.
    Tab activated{host_.scriptEngine, sub};
    auto livePanes = activated.panes();
    if (livePanes.empty()) {
        host_.scriptEngine->setFocusedTerminalNodeId({});
        return;
    }
    Uuid focus = host_.scriptEngine->focusedTerminalNodeId();
    bool focusInTab = false;
    for (Terminal* t : livePanes) if (t && t->nodeId() == focus) { focusInTab = true; break; }
    if (!focusInTab) {
        host_.scriptEngine->setFocusedTerminalNodeId(livePanes.front()->nodeId());
    }
}

void TabManager::attachLayoutSubtree(Tab tab, bool activate)
{
    if (!tab || !host_.scriptEngine) return;
    LayoutTree& tree = host_.scriptEngine->layoutTree();
    Uuid rootStack = host_.scriptEngine->layoutRootStack();
    Uuid sub = tab.subtreeRoot();
    if (sub.isNil()) return;

    tree.appendChild(rootStack, ChildSlot{sub, /*stretch=*/1});
    if (activate) tree.setActiveChild(rootStack, sub);
}

void TabManager::addInitialTab(Tab tab)
{
    attachLayoutSubtree(tab, /*activate=*/true);
}


void TabManager::updateWindowTitle()
{
    if (host_.headless && host_.headless()) return;
    auto t = activeTab();
    if (!t) return;
    Terminal* fp = t->focusedPane();
    if (!fp) return;
    const std::string& icon = fp->icon();
    // Prefer the pane's OSC-set title; fall back to the tab title (e.g. foreground process name)
    const std::string& title = fp->title().empty() ? t->title() : fp->title();
    if (title.empty()) return;
    std::string windowTitle = icon.empty() ? title : icon + " " + title;
    Window* win = host_.window ? host_.window() : nullptr;
    if (win) win->setTitle(windowTitle);
}


void TabManager::addPtyPoll(int fd, Terminal* term)
{
    EventLoop* loop = host_.eventLoop ? host_.eventLoop() : nullptr;
    term->setLoop(loop);
    if (loop) {
        loop->watchFd(fd, EventLoop::FdEvents::Readable,
            [term](EventLoop::FdEvents ev) {
                if (ev & EventLoop::FdEvents::Readable) term->readFromFD();
                if (ev & EventLoop::FdEvents::Writable) term->flushWriteQueue();
            });
    }
    ptyPolls_[fd] = term;
}


void TabManager::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) return;
    if (EventLoop* loop = host_.eventLoop ? host_.eventLoop() : nullptr)
        loop->removeFd(fd);
    ptyPolls_.erase(it);
}


void TabManager::refreshDividers(Tab tab)
{
    if (!tab) return;
    int divPx = host_.dividerWidth ? host_.dividerWidth() : 0;
    if (divPx <= 0) return;

    // Clear all divider VBs for this tab's panes
    auto lanes = tab.panes();
    for (Terminal* panePtr : lanes)
        host_.pending->clearDividerPanes.push_back(panePtr->id());

    if (lanes.size() <= 1) return;
    // When zoomed, dividersWithOwnerPanes returns empty naturally (non-zoomed
    // sibling rects aren't in the map, so dividersIn finds nothing to emit).

    // Collect (paneId, dividerRect) for each split boundary in the tree.
    auto dividers = tab.dividersWithOwnerPanes(divPx);

    host_.pending->dividersDirty = true;

    const float dr = host_.dividerR ? host_.dividerR() : 0.0f;
    const float dg = host_.dividerG ? host_.dividerG() : 0.0f;
    const float db = host_.dividerB ? host_.dividerB() : 0.0f;
    const float da = host_.dividerA ? host_.dividerA() : 1.0f;

    for (auto& [paneId, dr2] : dividers) {
        PendingMutations::DividerUpdate du;
        du.paneId = paneId;
        du.x = static_cast<float>(dr2.x);
        du.y = static_cast<float>(dr2.y);
        du.w = static_cast<float>(dr2.w);
        du.h = static_cast<float>(dr2.h);
        du.r = dr; du.g = dg;
        du.b = db; du.a = da;
        du.valid = true;
        host_.pending->dividerUpdates.push_back(du);
    }
}


void TabManager::clearDividers(Tab tab)
{
    if (!tab) return;
    for (Terminal* panePtr : tab.panes())
        host_.pending->clearDividerPanes.push_back(panePtr->id());
    host_.pending->dividersDirty = true;
}


void TabManager::releaseTabTextures(Tab tab)
{
    if (!tab) return;
    for (Terminal* panePtr : tab.panes()) {
        host_.pending->releasePaneTextures.push_back(panePtr->id());
        host_.pending->dirtyPanes.insert(panePtr->id());
    }
}


void TabManager::notifyPaneFocusChange(Tab tab, Uuid prevId, Uuid newId)
{
    if (!tab) return;
    if (!prevId.isNil()) {
        Terminal* p = tab.pane(prevId);
        if (p) {
            if (!p->focusedPopupId().empty() && host_.scriptEngine)
                host_.scriptEngine->notifyFocusedPopupChanged(prevId, "");
            p->clearFocusedPopup();
            p->focusEvent(false);
        }
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(prevId, false);
    }
    if (!newId.isNil()) {
        Terminal* p = tab.pane(newId);
        if (p) p->focusEvent(true);
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(newId, true);
    }
    if (host_.inputController) host_.inputController->refreshPointerShape();
}


void TabManager::updateTabTitleFromFocusedPane(int tabIdx)
{
    auto tab = tabAt(tabIdx);
    if (!tab) return;
    Terminal* fp = tab->focusedPane();
    if (!fp) return;

    const std::string& title = fp->title();
    const std::string& icon  = fp->icon();
    tab->setTitle(title);
    if (!icon.empty()) tab->setIcon(icon);
    if (tabIdx == activeTabIdx() && !title.empty())
        updateWindowTitle();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}


void TabManager::closeTab(int idx)
{
    auto tab = tabAt(idx);
    if (!tab) return;
    if (size() == 1) return; // can't close the last tab
    if (!tab->valid()) return;

    // Stop PTY polls for all terminals in this tab. Queue render state
    // cleanup and fire script notifications before the tab leaves its
    // slot — scripts and structural-op consumers observe the pre-destroy
    // state, while the actual C++ destruction is deferred via the
    // graveyard below.
    auto tabPanes = tab->panes();
    for (Terminal* panePtr : tabPanes) {
        removePtyPoll(panePtr->masterFD());
        host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{panePtr->id()});
        for (const auto& popup : panePtr->popups()) {
            std::string key = popupStateKey(panePtr->id(), popup->popupId());
            host_.pending->releasePopupTextures.push_back(key);
        }
        if (host_.inputController) host_.inputController->erasePaneCursorStyle(panePtr->id());
        if (host_.scriptEngine)
            host_.scriptEngine->notifyPaneDestroyed(panePtr->id(), panePtr->nodeId());
    }

    if (host_.scriptEngine)
        host_.scriptEngine->notifyTabDestroyed(idx, tab->subtreeRoot());

    // Extract the Layout + its Terminals + overlays under the render-thread
    // mutex, rebuild the shadow copy so the new active tab is visible, and
    // stage everything into the graveyard with the same stamp. The render
    // thread may still hold raw Terminal pointers from an in-flight frame;
    // the stamp waits for that frame to end before destructors run.
    Uuid subRoot = tab->subtreeRoot();
    std::vector<std::unique_ptr<Terminal>> extractedTerminals;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::recursive_mutex> plk(*host_.platformMutex);
        if (host_.scriptEngine) {
            for (Terminal* panePtr : tabPanes) {
                if (!panePtr) continue;
                auto t = host_.scriptEngine->extractTerminal(panePtr->nodeId());
                if (t) extractedTerminals.push_back(std::move(t));
            }
            host_.scriptEngine->eraseTabIcon(subRoot);
            // Detach the tab's subtree from the root Stack and destroy it.
            // No Layout object to graveyard — the tree nodes are the only
            // structural state left, destroyed immediately under the mutex
            // (render thread reads the shadow copy, not the tree).
            // destroyNode also clears any Stack zoomTarget that pointed
            // into this subtree.
            LayoutTree& tree = host_.scriptEngine->layoutTree();
            tree.removeChild(host_.scriptEngine->layoutRootStack(), subRoot);
            tree.destroyNode(subRoot);
            // Clear focus if it pointed into the destroyed subtree.
            host_.scriptEngine->setFocusedTerminalNodeId({});

            // Activate a surviving tab (prefer the one before the closed
            // index, else the first). Without this the root Stack's
            // activeChild is nil after removeChild and downstream lookups
            // (active tab, focused pane) break — tests that split_pane
            // after a close would silently no-op.
            int surviving = (idx > 0) ? (idx - 1) : 0;
            Uuid newActive = tabSubtreeRootAt(surviving);
            if (!newActive.isNil()) {
                tree.setActiveChild(host_.scriptEngine->layoutRootStack(), newActive);
                Tab newTab{host_.scriptEngine, newActive};
                auto livePanes = newTab.panes();
                if (!livePanes.empty())
                    host_.scriptEngine->setFocusedTerminalNodeId(livePanes.front()->nodeId());
            }
        }
        if (host_.buildRenderFrameState) host_.buildRenderFrameState();
        stamp = host_.completedFrames ? host_.completedFrames() : 0;
    }
    if (host_.graveyard) {
        for (auto& t : extractedTerminals)
            host_.graveyard->defer(std::move(t), stamp);
    }

    if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
    if (host_.inputController) host_.inputController->refreshPointerShape();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
    spdlog::info("Closed tab {}", idx + 1);
}


void TabManager::terminalExited(Terminal* terminal)
{
    // Called from drainPendingExits() under the render-thread mutex, so
    // the render thread cannot be snapshotting while we mutate live state.
    // The platformMutex is already held by our caller.
    //
    // Shell-exit path: the PTY is closed, the child has exited. Kill the
    // Terminal synchronously (extract + graveyard + fire event) and let JS
    // decide whether to remove the tree node, close the tab, or quit.
    if (!terminal) return;
    killTerminal(terminal->nodeId());
}

bool TabManager::killTerminal(Uuid nodeId)
{
    // Caller must hold host_.platformMutex — this mutates live state the
    // render thread observes through the shadow copy.
    if (nodeId.isNil() || !host_.scriptEngine) return false;
    Terminal* terminal = host_.scriptEngine->terminal(nodeId);
    if (!terminal) return false; // already killed or never inserted

    // Release PTY poll + render-state slots. Uuid is the sole pane identity.
    removePtyPoll(terminal->masterFD());
    host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{nodeId});
    for (const auto& popup : terminal->popups()) {
        std::string key = popupStateKey(nodeId, popup->popupId());
        host_.pending->releasePopupTextures.push_back(key);
    }
    if (host_.inputController) host_.inputController->erasePaneCursorStyle(nodeId);

    // Transfer ownership out of the engine map, rebuild the render-thread
    // shadow copy so the now-dead Terminal isn't observed next frame, stamp
    // with the completed-frame counter, and graveyard. The tree node stays;
    // JS will remove it (or keep it) in response to the event.
    std::unique_ptr<Terminal> extracted = host_.scriptEngine->extractTerminal(nodeId);
    if (host_.buildRenderFrameState) host_.buildRenderFrameState();
    uint64_t stamp = host_.completedFrames ? host_.completedFrames() : 0;
    if (extracted && host_.graveyard)
        host_.graveyard->defer(std::move(extracted), stamp);

    // Fire the event after extract+graveyard so the invariant holds for JS:
    // "Terminal is graveyarded, tree node is still present."
    host_.scriptEngine->notifyTerminalExited(nodeId, nodeId);

    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
    return true;
}


void TabManager::spawnTerminalForPane(Uuid nodeId, int tabIdx, const std::string& cwd)
{
    const float charWidth  = host_.charWidth ? host_.charWidth() : 0.0f;
    const float lineHeight = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const float padLeft    = host_.padLeft ? host_.padLeft() : 0.0f;
    const float padTop     = host_.padTop ? host_.padTop() : 0.0f;
    const float padRight   = host_.padRight ? host_.padRight() : 0.0f;
    const float padBottom  = host_.padBottom ? host_.padBottom() : 0.0f;

    auto cbs = host_.buildTerminalCallbacks(nodeId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) {
        if (host_.queueTerminalExit) host_.queueTerminalExit(t);
    };
    pcbs.quit = [this]() { if (host_.quit) host_.quit(); };
    Script::Engine* scriptEngine = host_.scriptEngine;
    pcbs.shouldFilterOutput = [scriptEngine, nodeId]() {
        return scriptEngine->hasPaneOutputFilters(nodeId);
    };
    pcbs.filterOutput = [scriptEngine, nodeId](std::string& data) {
        scriptEngine->filterPaneOutput(nodeId, data);
    };
    pcbs.shouldFilterInput = [scriptEngine, nodeId]() {
        return scriptEngine->hasPaneInputFilters(nodeId);
    };
    pcbs.filterInput = [scriptEngine, nodeId](std::string& data) {
        scriptEngine->filterPaneInput(nodeId, data);
    };
    auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    auto opts = terminalOptions_;
    if (!cwd.empty()) opts.cwd = cwd;

    terminal->applyColorScheme(opts.colors);
    terminal->applyCursorConfig(opts.cursor);
    if (!terminal->init(opts)) {
        spdlog::error("spawnTerminalForPane: failed to init terminal");
        return;
    }

    // Read geometry from the layout tree node (Terminal may not exist yet).
    PaneRect pr;
    Tab ownerTab;
    auto allTabs = tabs();
    for (Tab& t : allTabs) {
        pr = t.nodeRect(nodeId);
        if (!pr.isEmpty()) { ownerTab = t; break; }
    }
    int cols = (pr.w > 0 && charWidth > 0)  ? static_cast<int>((pr.w - padLeft - padRight) / charWidth)  : 80;
    int rows = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    host_.pending->structuralOps.push_back(PendingMutations::CreatePaneState{nodeId, cols, rows});
    host_.pending->dirtyPanes.insert(nodeId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();

    if (ownerTab) {
        ownerTab.insertTerminal(nodeId, std::move(terminal));
    }
    addPtyPoll(masterFD, termPtr);

    if (host_.scriptEngine) host_.scriptEngine->notifyPaneCreated(tabIdx, nodeId);
}


void TabManager::resizeAllPanesInTab(Tab tab)
{
    if (!tab) return;

    clearDividers(tab);

    const float charWidth  = host_.charWidth ? host_.charWidth() : 0.0f;
    const float lineHeight = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const float padLeft    = host_.padLeft ? host_.padLeft() : 0.0f;
    const float padTop     = host_.padTop ? host_.padTop() : 0.0f;
    const float padRight   = host_.padRight ? host_.padRight() : 0.0f;
    const float padBottom  = host_.padBottom ? host_.padBottom() : 0.0f;
    const uint32_t fbWidth  = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbHeight = host_.fbHeight ? host_.fbHeight() : 0;

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight)));
    tab.computeRects(fbWidth, fbHeight, cellW, cellH);

    for (Terminal* pane : tab.panes()) {
        pane->resizeToRect(charWidth, lineHeight, padLeft, padTop, padRight, padBottom);

        int cols = std::max(pane->width(),  1);
        int rows = std::max(pane->height(), 1);
        Uuid id = pane->nodeId();

        host_.pending->structuralOps.push_back(
            PendingMutations::ResizePaneState{id, cols, rows});
        host_.pending->dirtyPanes.insert(id);
        host_.pending->releasePaneTextures.push_back(id);

        // Terminal::resize() sets mResizePending if dims changed;
        // flushPendingResize() will send TIOCSWINSZ in the render loop.
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneResized(id, cols, rows);
    }
    refreshDividers(tab);
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}

// -------------------------------------------------------------------------
// JS-facing primitives
// -------------------------------------------------------------------------

std::optional<Tab> TabManager::findTabBySubtreeRoot(Uuid subtreeRoot, int* outTabIdx) const
{
    auto allTabs = tabs();
    for (int i = 0; i < static_cast<int>(allTabs.size()); ++i) {
        if (allTabs[i].subtreeRoot() == subtreeRoot) {
            if (outTabIdx) *outTabIdx = i;
            return allTabs[i];
        }
    }
    return std::nullopt;
}

std::optional<Tab> TabManager::findTabForNode(Uuid nodeId, int* outTabIdx) const
{
    if (!host_.scriptEngine) return std::nullopt;
    Uuid sub = host_.scriptEngine->findTabSubtreeRootForNode(nodeId);
    if (sub.isNil()) return std::nullopt;
    if (outTabIdx) {
        auto roots = host_.scriptEngine->tabSubtreeRoots();
        for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
            if (roots[i] == sub) { *outTabIdx = i; break; }
        }
    }
    return Tab{host_.scriptEngine, sub};
}

int TabManager::createEmptyTab(Uuid* outNodeId)
{
    const bool headless = host_.headless && host_.headless();
    Window* win = host_.window ? host_.window() : nullptr;
    if (!win && !headless) return -1;

    const int divW = host_.dividerWidth ? host_.dividerWidth() : 0;

    Tab layout = Tab::newSubtree(host_.scriptEngine);
    layout.setDividerPixels(divW);
    // Tab-bar height used to be stored on the engine via setTabBar; it's now
    // a slot size on a TabBar node in the tree (set by default-ui.js at
    // bootstrap). Nothing per-tab to do here.

    Uuid subRoot = layout.subtreeRoot();
    if (outNodeId) *outNodeId = subRoot;

    attachLayoutSubtree(layout, /*activate=*/false);
    int tabIdx = -1;
    findTabBySubtreeRoot(subRoot, &tabIdx);

    if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();

    if (host_.scriptEngine) host_.scriptEngine->notifyTabCreated(tabIdx);
    return tabIdx;
}

void TabManager::activateTabByIdx(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(size())) return;
    if (auto prev = activeTab()) {
        clearDividers(*prev);
        releaseTabTextures(*prev);
    }
    setActiveTabIdx(idx);
    if (auto now = activeTab()) refreshDividers(*now);
    updateWindowTitle();
    if (host_.inputController) host_.inputController->refreshPointerShape();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}

bool TabManager::createTerminalInContainer(Uuid parentContainerNodeId,
                                           const std::string& cwd,
                                           Uuid* outNodeId)
{
    if (!host_.scriptEngine) return false;
    LayoutTree& tree = host_.scriptEngine->layoutTree();

    int tabIdx = -1;
    auto tab = findTabForNode(parentContainerNodeId, &tabIdx);
    if (!tab || !tab->valid()) return false;

    // If the caller passed the tab's subtreeRoot (a Stack), drill into the
    // Stack's activeChild — that's the content Container where panes live.
    Uuid attachParent = parentContainerNodeId;
    if (const Node* n = tree.node(attachParent)) {
        if (auto* sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) attachParent = sd->activeChild;
        }
    }

    Uuid newNodeId = tab->allocatePaneNode();
    if (newNodeId.isNil()) return false;
    if (!tree.appendChild(attachParent, ChildSlot{newNodeId, /*stretch=*/1}))
        return false;

    const uint32_t fbW = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbH = host_.fbHeight ? host_.fbHeight() : 0;
    const float cwF = host_.charWidth ? host_.charWidth() : 0.0f;
    const float chF = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const int cellW = std::max(1, static_cast<int>(std::round(cwF)));
    const int cellH = std::max(1, static_cast<int>(std::round(chF)));
    tab->computeRects(fbW, fbH, cellW, cellH);

    spawnTerminalForPane(newNodeId, tabIdx, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool TabManager::splitPaneByNodeId(Uuid existingPaneNodeId, LayoutNode::Dir dir,
                                   float ratio, bool newIsFirst,
                                   Uuid* outNodeId)
{
    (void)ratio; // splitByNodeId uses default stretch inheritance
    int tabIdx = -1;
    auto tab = findTabForNode(existingPaneNodeId, &tabIdx);
    if (!tab || !tab->valid()) return false;

    Uuid newNodeId = tab->allocatePaneNode();
    if (newNodeId.isNil()) return false;
    if (!tab->splitByNodeId(existingPaneNodeId, dir, newNodeId, newIsFirst))
        return false;

    const uint32_t fbW = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbH = host_.fbHeight ? host_.fbHeight() : 0;
    const float cwF = host_.charWidth ? host_.charWidth() : 0.0f;
    const float chF = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const int cellW = std::max(1, static_cast<int>(std::round(cwF)));
    const int cellH = std::max(1, static_cast<int>(std::round(chF)));
    tab->computeRects(fbW, fbH, cellW, cellH);

    std::string cwd;
    if (Terminal* fp = tab->pane(existingPaneNodeId))
        cwd = paneProcessCWD(fp);

    spawnTerminalForPane(newNodeId, tabIdx, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool TabManager::focusPaneById(Uuid nodeId)
{
    int tabIdx = -1;
    auto tab = findTabForPane(nodeId, &tabIdx);
    if (!tab || !tab->valid()) return false;
    Uuid prev = tab->focusedPaneId();
    tab->setFocusedPane(nodeId);
    notifyPaneFocusChange(*tab, prev, nodeId);
    updateTabTitleFromFocusedPane(tabIdx);
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
    return true;
}

bool TabManager::removeNode(Uuid nodeId)
{
    int tabIdx = -1;
    auto tab = findTabForNode(nodeId, &tabIdx);
    if (!tab || !tab->valid()) return false;
    if (nodeId == tab->subtreeRoot()) return false;

    // Tree mutation under the platform mutex so the render thread's next
    // frame doesn't snapshot a partially-removed subtree. Layout::
    // removeNodeSubtree enforces the "no live Terminals beneath" guard; if
    // it refuses, we haven't touched anything and can return false cleanly.
    bool removed = false;
    {
        std::lock_guard<std::recursive_mutex> plk(*host_.platformMutex);
        removed = tab->removeNodeSubtree(nodeId);
        if (removed && host_.buildRenderFrameState) host_.buildRenderFrameState();
    }
    if (!removed) return false;

    // Skip resize/focus chrome when the tab is now empty — there is nothing
    // to size or focus, and the controller will shortly close the tab.
    auto livePanes = tab->panes();
    if (livePanes.empty()) {
        if (host_.setNeedsRedraw) host_.setNeedsRedraw();
        return true;
    }

    // If the removed subtree contained the focused Terminal,
    // Tab::removeNodeSubtree cleared the engine's focus. Promote focus to
    // the first remaining pane so the user isn't stuck with no focus.
    if (tab->focusedPaneId().isNil()) {
        tab->setFocusedPane(livePanes.front()->nodeId());
    }

    resizeAllPanesInTab(*tab);
    notifyPaneFocusChange(*tab, Uuid{}, tab->focusedPaneId());
    updateTabTitleFromFocusedPane(tabIdx);
    return true;
}
