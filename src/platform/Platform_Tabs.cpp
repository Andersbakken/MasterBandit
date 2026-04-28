#include "PlatformDawn.h"
#include "Graveyard.h"
#include "InputController.h"
#include "PlatformUtils.h"
#include "RenderThread.h"
#include "ScriptEngine.h"
#include "Terminal.h"
#include "Utils.h"

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
// ---------------------------------------------------------------------------

Uuid PlatformDawn::tabSubtreeRootAt(int idx) const
{
    if (idx < 0) return {};
    auto roots = scriptEngine_.tabSubtreeRoots();
    if (idx >= static_cast<int>(roots.size())) return {};
    return roots[idx];
}

std::optional<Uuid> PlatformDawn::activeTab() const
{
    return tabAt(scriptEngine_.activeTabIndex());
}

std::optional<Uuid> PlatformDawn::tabAt(int idx) const
{
    Uuid sub = tabSubtreeRootAt(idx);
    if (sub.isNil()) return std::nullopt;
    return sub;
}

Terminal* PlatformDawn::activeTerm()
{
    auto tab = activeTab();
    if (!tab) return nullptr;
    Terminal* pane = scriptEngine_.focusedTerminalInSubtree(*tab);
    return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
}

void PlatformDawn::notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn)
{
    for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
        for (Terminal* panePtr : scriptEngine_.panesInSubtree(sub)) fn(panePtr);
    }
}

std::optional<Uuid> PlatformDawn::findTabForPane(Uuid nodeId) const
{
    // hasPaneSlotInSubtree (not paneInSubtree) so a killed-but-not-yet-removed
    // Terminal's enclosing Tab is still resolvable — the controller needs it
    // to drive tree removal in response to `terminalExited`.
    for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
        if (scriptEngine_.hasPaneSlotInSubtree(sub, nodeId)) return sub;
    }
    return std::nullopt;
}

void PlatformDawn::setActiveTab(Uuid sub)
{
    if (sub.isNil()) return;
    scriptEngine_.layoutTree().setActiveChild(scriptEngine_.layoutRootStack(), sub);

    // The engine holds a single focused-terminal Uuid. On tab activation we
    // want to land on *this* tab's last-focused pane (so switching away and
    // back restores the pane the user was using). Fall back to the existing
    // behavior when there's no memory yet: keep focus if it's already in the
    // tab, else pick the first live pane. Without the fallback, JS actions
    // reading `mb.layout.focusedPane()` right after a tab switch would see
    // null focus and no-op.
    auto livePanes = scriptEngine_.panesInSubtree(sub);
    if (livePanes.empty()) {
        scriptEngine_.setFocusedTerminalNodeId({});
        return;
    }
    Uuid remembered = scriptEngine_.rememberedFocusInSubtree(sub);
    if (!remembered.isNil()) {
        scriptEngine_.setFocusedTerminalNodeId(remembered);
        return;
    }
    Uuid focus = scriptEngine_.focusedTerminalNodeId();
    bool focusInTab = false;
    for (Terminal* t : livePanes) if (t && t->nodeId() == focus) { focusInTab = true; break; }
    if (!focusInTab) {
        scriptEngine_.setFocusedTerminalNodeId(livePanes.front()->nodeId());
    }
}

void PlatformDawn::attachLayoutSubtree(Uuid subtreeRoot, bool activate)
{
    if (subtreeRoot.isNil()) return;
    LayoutTree& tree = scriptEngine_.layoutTree();
    Uuid rootStack = scriptEngine_.layoutRootStack();

    tree.appendChild(rootStack, ChildSlot{subtreeRoot, /*stretch=*/1});
    if (activate) tree.setActiveChild(rootStack, subtreeRoot);
}

void PlatformDawn::updateWindowTitle()
{
    if (isHeadless()) return;
    auto tab = activeTab();
    if (!tab) return;
    // Use the tab's remembered focus so a just-switched tab doesn't briefly
    // resolve via the global-focus chain.
    Terminal* fp = scriptEngine_.rememberedFocusTerminalInSubtree(*tab);
    if (!fp) return;
    // Resolution order for the title: JS-set label on the tab node wins,
    // then the pane's OSC title, then the pane's foreground process. An
    // empty string at any level falls through (apps often clear titles
    // with OSC 2 "").
    std::string title = scriptEngine_.tabTitle(*tab);
    if (title.empty()) {
        auto t = fp->title();
        if (t.has_value() && !t->empty()) title = *t;
        else                              title = fp->foregroundProcess();
    }
    if (title.empty()) return;
    std::string iconStr = fp->icon().value_or("");
    std::string windowTitle = iconStr.empty() ? title : iconStr + " " + title;
    if (window_) window_->setTitle(windowTitle);
}

void PlatformDawn::addPtyPoll(int fd, Terminal* term)
{
    EventLoop* loop = eventLoop_.get();
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

void PlatformDawn::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) return;
    if (EventLoop* loop = eventLoop_.get())
        loop->removeFd(fd);
    ptyPolls_.erase(it);
}

void PlatformDawn::refreshDividers(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) return;
    int divPx = dividerWidth_;
    if (divPx <= 0) return;

    PendingMutations& pending = renderThread_->pending();

    // Clear all divider VBs for this tab's panes
    auto lanes = scriptEngine_.panesInSubtree(subtreeRoot);
    for (Terminal* panePtr : lanes)
        pending.clearDividerPanes.push_back(panePtr->id());

    if (lanes.size() <= 1) return;
    // When zoomed, tabDividersWithOwnerPanes returns empty naturally
    // (non-zoomed sibling rects aren't in the map, so dividersIn finds
    // nothing to emit).

    auto dividers = scriptEngine_.tabDividersWithOwnerPanes(subtreeRoot, divPx);

    pending.dividersDirty = true;

    const float dr = dividerR_;
    const float dg = dividerG_;
    const float db = dividerB_;
    const float da = dividerA_;

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
        pending.dividerUpdates.push_back(du);
    }
}

void PlatformDawn::clearDividers(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) return;
    PendingMutations& pending = renderThread_->pending();
    for (Terminal* panePtr : scriptEngine_.panesInSubtree(subtreeRoot))
        pending.clearDividerPanes.push_back(panePtr->id());
    pending.dividersDirty = true;
}

void PlatformDawn::releaseTabTextures(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) return;
    PendingMutations& pending = renderThread_->pending();
    for (Terminal* panePtr : scriptEngine_.panesInSubtree(subtreeRoot)) {
        pending.releasePaneTextures.push_back(panePtr->id());
        pending.dirtyPanes.insert(panePtr->id());
    }
}

void PlatformDawn::notifyPaneFocusChange(Uuid subtreeRoot, Uuid prevId, Uuid newId)
{
    if (subtreeRoot.isNil()) return;
    if (!prevId.isNil()) {
        Terminal* p = scriptEngine_.paneInSubtree(subtreeRoot, prevId);
        if (p) {
            if (!p->focusedPopupId().empty())
                scriptEngine_.notifyFocusedPopupChanged(prevId, "");
            p->clearFocusedPopup();
            p->focusEvent(false);
        }
        scriptEngine_.notifyPaneFocusChanged(prevId, false);
    }
    if (!newId.isNil()) {
        Terminal* p = scriptEngine_.paneInSubtree(subtreeRoot, newId);
        if (p) p->focusEvent(true);
        scriptEngine_.notifyPaneFocusChanged(newId, true);
    }
    if (inputController_) inputController_->refreshPointerShape();
}

void PlatformDawn::closeTab(Uuid subRoot)
{
    if (subRoot.isNil()) return;
    if (scriptEngine_.tabCount() == 1) return; // can't close the last tab

    // Find the closed tab's position in the root Stack's children so we can
    // pick a sensible surviving sibling after removal. Bail out if the UUID
    // doesn't actually identify a tab (caller passed garbage).
    auto roots = scriptEngine_.tabSubtreeRoots();
    int idx = -1;
    for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
        if (roots[i] == subRoot) { idx = i; break; }
    }
    if (idx < 0) return;

    PendingMutations& pending = renderThread_->pending();

    auto tabPanes = scriptEngine_.panesInSubtree(subRoot);
    for (Terminal* panePtr : tabPanes) {
        removePtyPoll(panePtr->masterFD());
        pending.structuralOps.push_back(PendingMutations::DestroyPaneState{panePtr->id()});
        for (const auto& popup : panePtr->popups()) {
            std::string key = popupStateKey(panePtr->id(), popup->popupId());
            pending.releasePopupTextures.push_back(key);
        }
        if (inputController_) inputController_->erasePaneCursorStyle(panePtr->id());
        scriptEngine_.notifyPaneDestroyed(panePtr->id(), panePtr->nodeId());
    }

    scriptEngine_.notifyTabDestroyed(subRoot);

    std::vector<std::unique_ptr<Terminal>> extractedTerminals;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
        for (Terminal* panePtr : tabPanes) {
            if (!panePtr) continue;
            auto t = scriptEngine_.extractTerminal(panePtr->nodeId());
            if (t) extractedTerminals.push_back(std::move(t));
        }
        scriptEngine_.eraseTabIcon(subRoot);
        scriptEngine_.eraseLastFocusedInTab(subRoot);
        LayoutTree& tree = scriptEngine_.layoutTree();
        tree.removeChild(scriptEngine_.layoutRootStack(), subRoot);
        tree.destroyNode(subRoot);
        scriptEngine_.setFocusedTerminalNodeId({});

        // Activate a surviving tab (prefer the one before the closed
        // position, else the first). Without this the root Stack's
        // activeChild is nil after removeChild and downstream lookups
        // (active tab, focused pane) break — tests that split_pane
        // after a close would silently no-op.
        int surviving = (idx > 0) ? (idx - 1) : 0;
        Uuid newActive = tabSubtreeRootAt(surviving);
        if (!newActive.isNil()) {
            tree.setActiveChild(scriptEngine_.layoutRootStack(), newActive);
            auto livePanes = scriptEngine_.panesInSubtree(newActive);
            if (!livePanes.empty()) {
                // Prefer this tab's last-focused pane over an arbitrary first
                // pane so users don't lose their working pane when a
                // neighboring tab closes.
                Uuid remembered = scriptEngine_.rememberedFocusInSubtree(newActive);
                scriptEngine_.setFocusedTerminalNodeId(
                    remembered.isNil() ? livePanes.front()->nodeId() : remembered);
            }
        }
        buildRenderFrameState();
        stamp = renderThread_->completedFrames();
    }
    for (auto& t : extractedTerminals)
        graveyard_.defer(std::move(t), stamp);

    updateTabBarVisibility();
    if (inputController_) inputController_->refreshPointerShape();
    tabBarDirty_ = true;
    setNeedsRedraw();
    spdlog::info("Closed tab {}", idx + 1);
}

void PlatformDawn::terminalExited(Terminal* terminal)
{
    // Called from renderThread_->drainPendingExits() under the render-thread mutex, so
    // the render thread cannot be snapshotting while we mutate live state.
    // Shell-exit path: the PTY is closed, the child has exited. Kill the
    // Terminal synchronously (extract + graveyard + fire event) and let JS
    // decide whether to remove the tree node, close the tab, or quit.
    if (!terminal) return;
    killTerminal(terminal->nodeId());
}

bool PlatformDawn::killTerminal(Uuid nodeId)
{
    // Caller must hold renderThread_->mutex() — this mutates live state
    // the render thread observes through the shadow copy.
    if (nodeId.isNil()) return false;
    Terminal* terminal = scriptEngine_.terminal(nodeId);
    if (!terminal) return false; // already killed or never inserted

    PendingMutations& pending = renderThread_->pending();

    removePtyPoll(terminal->masterFD());
    pending.structuralOps.push_back(PendingMutations::DestroyPaneState{nodeId});
    for (const auto& popup : terminal->popups()) {
        std::string key = popupStateKey(nodeId, popup->popupId());
        pending.releasePopupTextures.push_back(key);
    }
    if (inputController_) inputController_->erasePaneCursorStyle(nodeId);

    std::unique_ptr<Terminal> extracted = scriptEngine_.extractTerminal(nodeId);
    buildRenderFrameState();
    uint64_t stamp = renderThread_->completedFrames();
    if (extracted)
        graveyard_.defer(std::move(extracted), stamp);

    scriptEngine_.notifyTerminalExited(nodeId, nodeId);

    setNeedsRedraw();
    return true;
}

void PlatformDawn::spawnTerminalForPane(Uuid nodeId, Uuid subtreeRoot, const std::string& cwd)
{
    const float charWidth  = charWidth_;
    const float lineHeight = lineHeight_;
    const float padLeft    = padLeft_;
    const float padTop     = padTop_;
    const float padRight   = padRight_;
    const float padBottom  = padBottom_;

    auto cbs = buildTerminalCallbacks(nodeId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) {
        renderThread_->enqueueTerminalExit(t);
    };
    pcbs.quit = [this]() { quit(); };
    Script::Engine* scriptEngine = &scriptEngine_;
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
    // Fire the JS "resized" event on any embedded whose cols got updated
    // as a side effect of the parent's resize. Explicit em.resize(rows)
    // from JS fires via the resizeEmbedded platform callback; this
    // handles the orthogonal case where the parent pane itself changed
    // width (split, tab-bar toggle, live-resize).
    terminal->onEmbeddedResized = [this, nodeId](uint64_t lineId, int cols, int rows) {
        scriptEngine_.deliverEmbeddedResized(nodeId, lineId, cols, rows);
    };
    auto opts = terminalOptions_;
    if (!cwd.empty()) opts.cwd = cwd;

    terminal->applyColorScheme(opts.colors);
    terminal->applyCursorConfig(opts.cursor);
    if (!terminal->init(opts)) {
        spdlog::error("spawnTerminalForPane: failed to init terminal");
        return;
    }

    // Locate the owning tab via findTabForNode — nodeId has already been
    // attached to its Container in the tree by the caller.
    Uuid ownerTab = scriptEngine_.findTabSubtreeRootForNode(nodeId);
    Rect pr = ownerTab.isNil() ? Rect{} : scriptEngine_.nodeRectInSubtree(ownerTab, nodeId);
    int cols = (pr.w > 0 && charWidth > 0)  ? static_cast<int>((pr.w - padLeft - padRight) / charWidth)  : 80;
    int rows = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    PendingMutations& pending = renderThread_->pending();
    pending.structuralOps.push_back(PendingMutations::CreatePaneState{nodeId, cols, rows});
    pending.dirtyPanes.insert(nodeId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();

    if (!ownerTab.isNil()) {
        terminal->setNodeId(nodeId);
        scriptEngine_.insertTerminal(nodeId, std::move(terminal));
    }
    addPtyPoll(masterFD, termPtr);

    scriptEngine_.notifyPaneCreated(subtreeRoot, nodeId);
}

void PlatformDawn::resizeAllPanesInTab(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) return;

    clearDividers(subtreeRoot);

    const float charWidth  = charWidth_;
    const float lineHeight = lineHeight_;
    const float padLeft    = padLeft_;
    const float padTop     = padTop_;
    const float padRight   = padRight_;
    const float padBottom  = padBottom_;

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight)));
    scriptEngine_.computeTabRects(subtreeRoot, fbWidth_, fbHeight_, cellW, cellH);

    PendingMutations& pending = renderThread_->pending();

    for (Terminal* pane : scriptEngine_.panesInSubtree(subtreeRoot)) {
        pane->resizeToRect(charWidth, lineHeight, padLeft, padTop, padRight, padBottom);

        int cols = std::max(pane->width(),  1);
        int rows = std::max(pane->height(), 1);
        Uuid id = pane->nodeId();

        pending.structuralOps.push_back(
            PendingMutations::ResizePaneState{id, cols, rows});
        pending.dirtyPanes.insert(id);
        pending.releasePaneTextures.push_back(id);

        scriptEngine_.notifyPaneResized(id, cols, rows);
    }
    refreshDividers(subtreeRoot);
    setNeedsRedraw();
}

std::optional<Uuid> PlatformDawn::findTabForNode(Uuid nodeId) const
{
    Uuid sub = scriptEngine_.findTabSubtreeRootForNode(nodeId);
    if (sub.isNil()) return std::nullopt;
    return sub;
}

Uuid PlatformDawn::createEmptyTab()
{
    const bool headless = isHeadless();
    if (!window_ && !headless) return {};

    Uuid subRoot = scriptEngine_.createTabSubtree();
    scriptEngine_.setDividerPixels(dividerWidth_);

    attachLayoutSubtree(subRoot, /*activate=*/false);

    updateTabBarVisibility();
    tabBarDirty_ = true;
    setNeedsRedraw();

    scriptEngine_.notifyTabCreated(subRoot);
    return subRoot;
}

void PlatformDawn::activateTabByUuid(Uuid sub)
{
    if (sub.isNil()) return;
    // Confirm the UUID is actually a tab — `setActiveChild` would silently
    // no-op otherwise and we'd skip the GPU/title teardown below.
    auto roots = scriptEngine_.tabSubtreeRoots();
    bool found = false;
    for (Uuid r : roots) if (r == sub) { found = true; break; }
    if (!found) return;
    if (auto prev = activeTab()) {
        clearDividers(*prev);
        releaseTabTextures(*prev);
    }
    setActiveTab(sub);
    if (auto now = activeTab()) refreshDividers(*now);
    updateWindowTitle();
    if (inputController_) inputController_->refreshPointerShape();
    tabBarDirty_ = true;
    setNeedsRedraw();
}

bool PlatformDawn::createTerminalInContainer(Uuid parentContainerNodeId,
                                             const std::string& cwdIn,
                                             Uuid* outNodeId)
{
    LayoutTree& tree = scriptEngine_.layoutTree();

    auto tab = findTabForNode(parentContainerNodeId);
    if (!tab) return false;

    Uuid attachParent = parentContainerNodeId;
    if (const Node* n = tree.node(attachParent)) {
        if (auto* sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) attachParent = sd->activeChild;
        }
    }

    Uuid newNodeId = scriptEngine_.allocatePaneNode();
    if (newNodeId.isNil()) return false;
    if (!tree.appendChild(attachParent, ChildSlot{newNodeId, /*stretch=*/1}))
        return false;

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth_)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight_)));
    scriptEngine_.computeTabRects(*tab, fbWidth_, fbHeight_, cellW, cellH);

    // Caller-supplied cwd wins; fall back to the globally-focused pane's
    // process cwd so user scripts that call mb.layout.createTerminal(tab)
    // without opts still get automatic inheritance from the user's
    // current pane (symmetric with splitPane).
    std::string cwd = cwdIn;
    if (cwd.empty()) {
        Uuid focusedNode = scriptEngine_.focusedTerminalNodeId();
        if (!focusedNode.isNil()) {
            if (Terminal* fp = scriptEngine_.terminal(focusedNode))
                cwd = paneProcessCWD(fp);
        }
    }

    spawnTerminalForPane(newNodeId, *tab, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool PlatformDawn::splitPaneByNodeId(Uuid existingPaneNodeId, SplitDir dir,
                                     float ratio, bool newIsFirst,
                                     const std::string& cwdIn,
                                     Uuid* outNodeId)
{
    (void)ratio;
    auto tab = findTabForNode(existingPaneNodeId);
    if (!tab) return false;

    Uuid newNodeId = scriptEngine_.allocatePaneNode();
    if (newNodeId.isNil()) return false;
    if (!scriptEngine_.splitByNodeId(existingPaneNodeId, dir, newNodeId, newIsFirst))
        return false;

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth_)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight_)));
    scriptEngine_.computeTabRects(*tab, fbWidth_, fbHeight_, cellW, cellH);

    // Caller-supplied cwd wins; fall back to paneProcessCWD so user
    // scripts that call mb.layout.splitPane(...) without opts still get
    // automatic inheritance.
    std::string cwd = cwdIn;
    if (cwd.empty()) {
        if (Terminal* fp = scriptEngine_.paneInSubtree(*tab, existingPaneNodeId))
            cwd = paneProcessCWD(fp);
    }

    spawnTerminalForPane(newNodeId, *tab, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool PlatformDawn::focusPaneById(Uuid nodeId)
{
    auto tab = findTabForPane(nodeId);
    if (!tab) return false;
    Uuid prev = scriptEngine_.focusedPaneInSubtree(*tab);
    scriptEngine_.setFocusedTerminalNodeId(nodeId);
    notifyPaneFocusChange(*tab, prev, nodeId);
    // Tab bar reads title/icon live off the focused pane, so just mark dirty.
    tabBarDirty_ = true;
    if (*tab == scriptEngine_.activeTabSubtreeRoot()) updateWindowTitle();
    setNeedsRedraw();
    return true;
}

bool PlatformDawn::removeNode(Uuid nodeId)
{
    auto tab = findTabForNode(nodeId);
    if (!tab) return false;
    if (nodeId == *tab) return false;

    bool removed = false;
    {
        std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
        removed = scriptEngine_.removeNodeSubtree(*tab, nodeId);
        if (removed) buildRenderFrameState();
    }
    if (!removed) return false;

    auto livePanes = scriptEngine_.panesInSubtree(*tab);
    if (livePanes.empty()) {
        setNeedsRedraw();
        return true;
    }

    if (scriptEngine_.focusedPaneInSubtree(*tab).isNil()) {
        scriptEngine_.setFocusedTerminalNodeId(livePanes.front()->nodeId());
    }

    resizeAllPanesInTab(*tab);
    notifyPaneFocusChange(*tab, Uuid{}, scriptEngine_.focusedPaneInSubtree(*tab));
    tabBarDirty_ = true;
    if (*tab == scriptEngine_.activeTabSubtreeRoot()) updateWindowTitle();
    setNeedsRedraw();
    return true;
}

// ---------------------------------------------------------------------------
// Terminal callbacks builder
// ---------------------------------------------------------------------------

TerminalCallbacks PlatformDawn::buildTerminalCallbacks(Uuid paneId)
{
    TerminalCallbacks cbs;

    cbs.event = [this, paneId](TerminalEmulator*, int ev, void* payload) {
        switch (static_cast<TerminalEmulator::Event>(ev)) {
        case TerminalEmulator::Update:
        case TerminalEmulator::ScrollbackChanged:
            setNeedsRedraw();
            eventLoop_->post([this, paneId] {
                renderThread_->pending().dirtyPanes.insert(paneId);
            });
            break;
        case TerminalEmulator::VisibleBell:
            break;
        case TerminalEmulator::CommandComplete:
            if (payload) {
                const auto* rec = static_cast<const TerminalEmulator::CommandRecord*>(payload);
                TerminalEmulator::CommandRecord recCopy = *rec;
                eventLoop_->post([this, paneId, recCopy = std::move(recCopy)] {
                    TerminalEmulator* te = scriptEngine_.terminal(paneId);
                    if (!te) return;
                    const auto& doc = te->document();
                    Script::CommandInfo info{
                        recCopy.id, recCopy.cwd, recCopy.exitCode,
                        recCopy.startMs, recCopy.endMs,
                        recCopy.promptStartLineId, recCopy.commandStartLineId,
                        recCopy.outputStartLineId, recCopy.outputEndLineId,
                        doc.firstAbsOfLine(recCopy.promptStartLineId),  recCopy.promptStartCol,
                        doc.firstAbsOfLine(recCopy.commandStartLineId), recCopy.commandStartCol,
                        doc.firstAbsOfLine(recCopy.outputStartLineId),  recCopy.outputStartCol,
                        doc.lastAbsOfLine(recCopy.outputEndLineId),     recCopy.outputEndCol
                    };
                    scriptEngine_.notifyCommandComplete(paneId, info);
                });
            }
            break;
        case TerminalEmulator::CommandSelectionChanged:
            eventLoop_->post([this, paneId] {
                Terminal* te = scriptEngine_.terminal(paneId);
                if (!te) return;
                scriptEngine_.notifyCommandSelectionChanged(paneId, te->selectedCommandId());
            });
            break;
        }
    };

    if (!isHeadless()) {
        cbs.copyToClipboard = [this](const std::string& text) {
            eventLoop_->post([this, text] {
                if (window_) window_->setClipboard(text);
            });
        };
        cbs.pasteFromClipboard = [this]() -> std::string {
            return window_ ? window_->getClipboard() : std::string{};
        };
    } else {
        cbs.copyToClipboard = [](const std::string&) {};
        cbs.pasteFromClipboard = []() -> std::string { return {}; };
    }

    // Pull-model: title/icon live on the emulator's XTWINOPS stack. These
    // callbacks only need to dirty the tab bar + refresh the window title
    // so the render thread re-reads. std::nullopt means "stack went empty"
    // (pop-to-empty); Some("") is an explicit OSC 2 "" — both treated the
    // same here since the pull side handles resolution.
    cbs.onTitleChanged = [this, paneId](std::optional<std::string>) {
        eventLoop_->post([this, paneId] {
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId) {
                if (*tab == scriptEngine_.activeTabSubtreeRoot()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onIconChanged = [this, paneId](std::optional<std::string>) {
        eventLoop_->post([this, paneId] {
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId) {
                if (*tab == scriptEngine_.activeTabSubtreeRoot()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        eventLoop_->post([this, paneId, state, pct] {
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            if (Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    // Captures `this` so the callback always reflects the current config
    // override (config.color_scheme = "auto" | "light" | "dark"); only
    // "auto" defers to the system query.
    cbs.isDarkMode = [this]() { return effectiveIsDarkMode(); };

    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        eventLoop_->post([this, paneId, dir] {
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            if (Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId)) p->setCWD(dir);
        });
    };

    if (isHeadless()) {
        cbs.onDesktopNotification    = [](const TerminalCallbacks::DesktopNotification&) {};
        cbs.onCloseNotification      = [](const std::string&) {};
        cbs.onQueryAliveNotifications = [](const std::string&) {};
    } else {
        // sourceTag = pane uuid stringified. clientId = OSC i= (may be
        // empty for un-tracked notifications, in which case the platform
        // skips replaces_id bookkeeping).
        cbs.onDesktopNotification = [this, paneId](const TerminalCallbacks::DesktopNotification& n) {
            std::string sourceTag = paneId.toString();
            // Capture the notification by value (struct holds vectors/strings).
            eventLoop_->post([this, paneId, sourceTag, n]() mutable {
                std::function<void(const std::string&)> onClosed;
                if (n.closeResponseRequested && !n.id.empty()) {
                    std::string clientId = n.id;
                    onClosed = [this, paneId, clientId](const std::string& reason) {
                        std::string osc = "\x1b]99;i=" + clientId
                                        + ":p=close;" + reason + "\a";
                        if (Terminal* term = scriptEngine_.terminal(paneId))
                            term->writeText(osc);
                    };
                }
                // onActivated fires when the user clicks the body or a button.
                // buttonId is empty for the body click, "1".."N" for buttons.
                std::function<void(const std::string&)> onActivated;
                if (n.actionFocus || n.actionReport) {
                    bool focusReq  = n.actionFocus;
                    bool reportReq = n.actionReport;
                    std::string clientId = n.id;
                    onActivated = [this, paneId, focusReq, reportReq, clientId]
                                   (const std::string& buttonId) {
                        if (focusReq) {
                            // Activate the tab that owns the pane (if not
                            // already active), focus the pane within it,
                            // then raise the window. Order matters: pane
                            // focus updates the per-tab "remembered focus"
                            // state, which the window's focus handler reads
                            // on activation.
                            if (auto tab = findTabForPane(paneId)) {
                                if (*tab != scriptEngine_.activeTabSubtreeRoot())
                                    setActiveTab(*tab);
                                focusPaneById(paneId);
                            }
                            if (window_) window_->raise();
                        }
                        if (reportReq && !clientId.empty()) {
                            std::string osc = "\x1b]99;i=" + clientId + ";"
                                            + buttonId + "\a";
                            if (Terminal* term = scriptEngine_.terminal(paneId))
                                term->writeText(osc);
                        }
                    };
                }
                platformSendNotification(sourceTag, n.id, n.title, n.body, n.urgency,
                                         n.closeResponseRequested, std::move(onClosed),
                                         n.buttons, std::move(onActivated), n.onlyWhen);
            });
        };

        cbs.onCloseNotification = [this, paneId](const std::string& clientId) {
            std::string sourceTag = paneId.toString();
            eventLoop_->post([sourceTag, clientId] {
                platformCloseNotification(sourceTag, clientId);
            });
        };

        cbs.onQueryAliveNotifications = [this, paneId](const std::string& responderId) {
            std::string sourceTag = paneId.toString();
            eventLoop_->post([this, paneId, sourceTag, responderId] {
                std::vector<std::string> alive = platformActiveNotifications(sourceTag);
                std::string csv;
                for (size_t i = 0; i < alive.size(); ++i) {
                    if (i) csv.push_back(',');
                    csv.append(alive[i]);
                }
                std::string osc = "\x1b]99;i=" + responderId + ":p=alive;" + csv + "\a";
                if (Terminal* term = scriptEngine_.terminal(paneId))
                    term->writeText(osc);
            });
        };
    }

    cbs.onOSC = [this, paneId](int oscNum, std::string_view payload) {
        std::string payloadCopy(payload);
        eventLoop_->post([this, paneId, oscNum, payloadCopy = std::move(payloadCopy)] {
            scriptEngine_.notifyOSC(paneId, oscNum, payloadCopy);
        });
    };

    cbs.customTcapLookup = [this](const std::string& name) -> std::optional<std::string> {
        return scriptEngine_.lookupCustomTcap(name);
    };

    cbs.onMouseCursorShape = [this, paneId](const std::string& shape) {
        eventLoop_->post([this, paneId, shape] {
            if (!inputController_) return;
            if (shape.empty()) {
                inputController_->erasePaneCursorStyle(paneId);
            } else {
                inputController_->setPaneCursorStyle(paneId,
                    InputController::pointerShapeNameToCursorStyle(shape));
            }
            if (!window_ || isHeadless()) return;
            auto tab = activeTab();
            if (!tab || scriptEngine_.focusedPaneInSubtree(*tab) != paneId) return;
            window_->setCursorStyle(shape.empty()
                ? Window::CursorStyle::IBeam
                : inputController_->paneCursorStyle(paneId));
        });
    };

    cbs.onForegroundProcessChanged = [this, paneId](const std::string& proc) {
        eventLoop_->post([this, paneId, proc] {
            scriptEngine_.notifyForegroundProcessChanged(paneId, proc);
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            // Pull-model: no tab-title cache to update. If this pane is the
            // tab's representative, the tab bar falls back to the fg process
            // when the pane has no OSC title — so just dirty the bar and
            // refresh the window title.
            if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId) {
                if (*tab == scriptEngine_.activeTabSubtreeRoot()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    return cbs;
}
