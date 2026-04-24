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

std::optional<Uuid> PlatformDawn::findTabForPane(Uuid nodeId, int* outTabIdx) const
{
    // hasPaneSlotInSubtree (not paneInSubtree) so a killed-but-not-yet-removed
    // Terminal's enclosing Tab is still resolvable — the controller needs it
    // to drive tree removal in response to `terminalExited`.
    auto allTabs = scriptEngine_.tabSubtreeRoots();
    for (int i = 0; i < static_cast<int>(allTabs.size()); ++i) {
        if (scriptEngine_.hasPaneSlotInSubtree(allTabs[i], nodeId)) {
            if (outTabIdx) *outTabIdx = i;
            return allTabs[i];
        }
    }
    return std::nullopt;
}

void PlatformDawn::setActiveTabIdx(int idx)
{
    Uuid sub = tabSubtreeRootAt(idx);
    if (sub.isNil()) return;
    scriptEngine_.layoutTree().setActiveChild(scriptEngine_.layoutRootStack(), sub);

    // The engine holds a single focused-terminal Uuid. If the currently
    // focused Terminal isn't inside the newly active tab, move focus onto
    // the first Terminal of that tab. Without this, closing a tab (which
    // clears focus if the focused pane was inside it) and splitting via a
    // JS action that reads `mb.layout.focusedPane()` would see null focus
    // and no-op the split.
    auto livePanes = scriptEngine_.panesInSubtree(sub);
    if (livePanes.empty()) {
        scriptEngine_.setFocusedTerminalNodeId({});
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
    Terminal* fp = scriptEngine_.focusedTerminalInSubtree(*tab);
    if (!fp) return;
    const std::string& icon = fp->icon();
    // Prefer the pane's OSC-set title; fall back to the tab title.
    const std::string& title = fp->title().empty() ? scriptEngine_.tabTitle(*tab) : fp->title();
    if (title.empty()) return;
    std::string windowTitle = icon.empty() ? title : icon + " " + title;
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

void PlatformDawn::updateTabTitleFromFocusedPane(int tabIdx)
{
    auto tab = tabAt(tabIdx);
    if (!tab) return;
    Terminal* fp = scriptEngine_.focusedTerminalInSubtree(*tab);
    if (!fp) return;

    const std::string& title = fp->title();
    const std::string& icon  = fp->icon();
    scriptEngine_.setTabTitle(*tab, title);
    if (!icon.empty()) scriptEngine_.setTabIcon(*tab, icon);
    if (tabIdx == scriptEngine_.activeTabIndex() && !title.empty())
        updateWindowTitle();
    tabBarDirty_ = true;
    setNeedsRedraw();
}

void PlatformDawn::closeTab(int idx)
{
    auto tab = tabAt(idx);
    if (!tab) return;
    if (scriptEngine_.tabCount() == 1) return; // can't close the last tab

    PendingMutations& pending = renderThread_->pending();

    auto tabPanes = scriptEngine_.panesInSubtree(*tab);
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

    scriptEngine_.notifyTabDestroyed(idx, *tab);

    Uuid subRoot = *tab;
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
        LayoutTree& tree = scriptEngine_.layoutTree();
        tree.removeChild(scriptEngine_.layoutRootStack(), subRoot);
        tree.destroyNode(subRoot);
        scriptEngine_.setFocusedTerminalNodeId({});

        // Activate a surviving tab (prefer the one before the closed
        // index, else the first). Without this the root Stack's
        // activeChild is nil after removeChild and downstream lookups
        // (active tab, focused pane) break — tests that split_pane
        // after a close would silently no-op.
        int surviving = (idx > 0) ? (idx - 1) : 0;
        Uuid newActive = tabSubtreeRootAt(surviving);
        if (!newActive.isNil()) {
            tree.setActiveChild(scriptEngine_.layoutRootStack(), newActive);
            auto livePanes = scriptEngine_.panesInSubtree(newActive);
            if (!livePanes.empty())
                scriptEngine_.setFocusedTerminalNodeId(livePanes.front()->nodeId());
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

void PlatformDawn::spawnTerminalForPane(Uuid nodeId, int tabIdx, const std::string& cwd)
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

    scriptEngine_.notifyPaneCreated(tabIdx, nodeId);
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

std::optional<Uuid> PlatformDawn::findTabForNode(Uuid nodeId, int* outTabIdx) const
{
    Uuid sub = scriptEngine_.findTabSubtreeRootForNode(nodeId);
    if (sub.isNil()) return std::nullopt;
    if (outTabIdx) {
        auto roots = scriptEngine_.tabSubtreeRoots();
        for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
            if (roots[i] == sub) { *outTabIdx = i; break; }
        }
    }
    return sub;
}

int PlatformDawn::createEmptyTab(Uuid* outNodeId)
{
    const bool headless = isHeadless();
    if (!window_ && !headless) return -1;

    Uuid subRoot = scriptEngine_.createTabSubtree();
    scriptEngine_.setDividerPixels(dividerWidth_);
    if (outNodeId) *outNodeId = subRoot;

    attachLayoutSubtree(subRoot, /*activate=*/false);
    int tabIdx = scriptEngine_.tabCount() - 1; // just appended to the end

    updateTabBarVisibility();
    tabBarDirty_ = true;
    setNeedsRedraw();

    scriptEngine_.notifyTabCreated(tabIdx);
    return tabIdx;
}

void PlatformDawn::activateTabByIdx(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(scriptEngine_.tabCount())) return;
    if (auto prev = activeTab()) {
        clearDividers(*prev);
        releaseTabTextures(*prev);
    }
    setActiveTabIdx(idx);
    if (auto now = activeTab()) refreshDividers(*now);
    updateWindowTitle();
    if (inputController_) inputController_->refreshPointerShape();
    tabBarDirty_ = true;
    setNeedsRedraw();
}

bool PlatformDawn::createTerminalInContainer(Uuid parentContainerNodeId,
                                             const std::string& cwd,
                                             Uuid* outNodeId)
{
    LayoutTree& tree = scriptEngine_.layoutTree();

    int tabIdx = -1;
    auto tab = findTabForNode(parentContainerNodeId, &tabIdx);
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

    spawnTerminalForPane(newNodeId, tabIdx, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool PlatformDawn::splitPaneByNodeId(Uuid existingPaneNodeId, SplitDir dir,
                                     float ratio, bool newIsFirst,
                                     Uuid* outNodeId)
{
    (void)ratio;
    int tabIdx = -1;
    auto tab = findTabForNode(existingPaneNodeId, &tabIdx);
    if (!tab) return false;

    Uuid newNodeId = scriptEngine_.allocatePaneNode();
    if (newNodeId.isNil()) return false;
    if (!scriptEngine_.splitByNodeId(existingPaneNodeId, dir, newNodeId, newIsFirst))
        return false;

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth_)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight_)));
    scriptEngine_.computeTabRects(*tab, fbWidth_, fbHeight_, cellW, cellH);

    std::string cwd;
    if (Terminal* fp = scriptEngine_.paneInSubtree(*tab, existingPaneNodeId))
        cwd = paneProcessCWD(fp);

    spawnTerminalForPane(newNodeId, tabIdx, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool PlatformDawn::focusPaneById(Uuid nodeId)
{
    int tabIdx = -1;
    auto tab = findTabForPane(nodeId, &tabIdx);
    if (!tab) return false;
    Uuid prev = scriptEngine_.focusedPaneInSubtree(*tab);
    scriptEngine_.setFocusedTerminalNodeId(nodeId);
    notifyPaneFocusChange(*tab, prev, nodeId);
    updateTabTitleFromFocusedPane(tabIdx);
    setNeedsRedraw();
    return true;
}

bool PlatformDawn::removeNode(Uuid nodeId)
{
    int tabIdx = -1;
    auto tab = findTabForNode(nodeId, &tabIdx);
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
    updateTabTitleFromFocusedPane(tabIdx);
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
            renderThread_->postToMain([this, paneId] {
                renderThread_->pending().dirtyPanes.insert(paneId);
            });
            break;
        case TerminalEmulator::VisibleBell:
            break;
        case TerminalEmulator::CommandComplete:
            if (payload) {
                const auto* rec = static_cast<const TerminalEmulator::CommandRecord*>(payload);
                TerminalEmulator::CommandRecord recCopy = *rec;
                renderThread_->postToMain([this, paneId, recCopy = std::move(recCopy)] {
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
            renderThread_->postToMain([this, paneId] {
                Terminal* te = scriptEngine_.terminal(paneId);
                if (!te) return;
                scriptEngine_.notifyCommandSelectionChanged(paneId, te->selectedCommandId());
            });
            break;
        }
    };

    if (!isHeadless()) {
        cbs.copyToClipboard = [this](const std::string& text) {
            renderThread_->postToMain([this, text] {
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

    cbs.onTitleChanged = [this, paneId](const std::string& title) {
        renderThread_->postToMain([this, paneId, title] {
            int tabIdx = -1;
            auto tab = findTabForPane(paneId, &tabIdx);
            if (!tab) return;
            if (Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId)) p->setTitle(title);
            if (scriptEngine_.focusedPaneInSubtree(*tab) == paneId) {
                scriptEngine_.setTabTitle(*tab, title);
                if (tabIdx == scriptEngine_.activeTabIndex()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onIconChanged = [this, paneId](const std::string& icon) {
        renderThread_->postToMain([this, paneId, icon] {
            int tabIdx = -1;
            auto tab = findTabForPane(paneId, &tabIdx);
            if (!tab) return;
            if (Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId)) p->setIcon(icon);
            if (scriptEngine_.focusedPaneInSubtree(*tab) == paneId) {
                scriptEngine_.setTabIcon(*tab, icon);
                if (tabIdx == scriptEngine_.activeTabIndex()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        renderThread_->postToMain([this, paneId, state, pct] {
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
    cbs.isDarkMode = isHeadless() ? []() { return true; } : []() { return platformIsDarkMode(); };

    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        renderThread_->postToMain([this, paneId, dir] {
            auto tab = findTabForPane(paneId);
            if (!tab) return;
            if (Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId)) p->setCWD(dir);
        });
    };

    if (isHeadless()) {
        cbs.onDesktopNotification = [](const std::string&, const std::string&, const std::string&) {};
    } else {
        cbs.onDesktopNotification = [this](const std::string& title, const std::string& body, const std::string& /*icon*/) {
            renderThread_->postToMain([title, body] {
                platformSendNotification(title, body);
            });
        };
    }

    cbs.onOSC = [this, paneId](int oscNum, std::string_view payload) {
        std::string payloadCopy(payload);
        renderThread_->postToMain([this, paneId, oscNum, payloadCopy = std::move(payloadCopy)] {
            scriptEngine_.notifyOSC(paneId, oscNum, payloadCopy);
        });
    };

    cbs.customTcapLookup = [this](const std::string& name) -> std::optional<std::string> {
        return scriptEngine_.lookupCustomTcap(name);
    };

    cbs.onMouseCursorShape = [this, paneId](const std::string& shape) {
        renderThread_->postToMain([this, paneId, shape] {
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
        renderThread_->postToMain([this, paneId, proc] {
            scriptEngine_.notifyForegroundProcessChanged(paneId, proc);
            int tabIdx = -1;
            auto tab = findTabForPane(paneId, &tabIdx);
            if (!tab) return;
            Terminal* p = scriptEngine_.paneInSubtree(*tab, paneId);
            if (p && p->title().empty() && scriptEngine_.focusedPaneInSubtree(*tab) == paneId) {
                scriptEngine_.setTabTitle(*tab, proc);
                if (tabIdx == scriptEngine_.activeTabIndex()) updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    return cbs;
}
