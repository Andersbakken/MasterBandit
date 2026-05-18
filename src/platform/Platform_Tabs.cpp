#include "Graveyard.h"
#include "InputController.h"
#include "PlatformDawn.h"
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
    if (idx < 0) {
        return { };
    }
    auto roots = scriptEngine_.tabSubtreeRoots();
    if (idx >= static_cast<int>(roots.size())) {
        return { };
    }
    return roots[idx];
}

std::optional<Uuid> PlatformDawn::activeTab() const
{
    return tabAt(scriptEngine_.activeTabIndex());
}

std::optional<Uuid> PlatformDawn::tabAt(int idx) const
{
    Uuid sub = tabSubtreeRootAt(idx);
    if (sub.isNil()) {
        return std::nullopt;
    }
    return sub;
}

Terminal *PlatformDawn::activeTerm()
{
    auto tab = activeTab();
    if (!tab) {
        return nullptr;
    }
    Terminal *pane = scriptEngine_.focusedTerminalInSubtree(*tab);
    return pane ? static_cast<Terminal *>(pane->activeTerm()) : nullptr;
}

void PlatformDawn::notifyAllTerminals(const std::function<void(TerminalEmulator *)> &fn)
{
    for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
        for (Terminal *panePtr : scriptEngine_.panesInSubtree(sub)) {
            fn(panePtr);
        }
    }
}

std::optional<Uuid> PlatformDawn::findTabForPane(Uuid nodeId) const
{
    // hasPaneSlotInSubtree (not paneInSubtree) so a killed-but-not-yet-removed
    // Terminal's enclosing Tab is still resolvable — the controller needs it
    // to drive tree removal in response to `terminalExited`.
    for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
        if (scriptEngine_.hasPaneSlotInSubtree(sub, nodeId)) {
            return sub;
        }
    }
    return std::nullopt;
}

void PlatformDawn::attachLayoutSubtree(Uuid subtreeRoot, bool activate)
{
    if (subtreeRoot.isNil()) {
        return;
    }
    LayoutTree &tree = scriptEngine_.layoutTree();
    Uuid rootStack   = scriptEngine_.layoutRootStack();

    tree.appendChild(rootStack, ChildSlot { subtreeRoot, /*stretch=*/1 });
    if (activate) {
        tree.setActiveChild(rootStack, subtreeRoot);
    }
}

void PlatformDawn::updateWindowTitle()
{
    if (isHeadless()) {
        return;
    }
    auto tab = activeTab();
    if (!tab) {
        return;
    }
    // Use the tab's remembered focus so a just-switched tab doesn't briefly
    // resolve via the global-focus chain.
    Terminal *fp = scriptEngine_.rememberedFocusTerminalInSubtree(*tab);
    if (!fp) {
        return;
    }
    // Resolution order for the title: JS-set label on the tab node wins,
    // then the pane's OSC title, then the pane's foreground process. An
    // empty string at any level falls through (apps often clear titles
    // with OSC 2 "").
    std::string title = scriptEngine_.tabTitle(*tab);
    if (title.empty()) {
        auto t = fp->title();
        if (t.has_value() && !t->empty()) {
            title = *t;
        } else {
            title = fp->foregroundProcess();
        }
    }
    if (title.empty()) {
        return;
    }
    std::string iconStr     = fp->icon().value_or("");
    std::string windowTitle = iconStr.empty() ? title : iconStr + " " + title;
    if (window_) {
        window_->setTitle(lastConfig_.window_title_prefix + windowTitle);
    }
}

void PlatformDawn::setDraggedTab(Uuid barId, Uuid tabId, float cursorX, float dragOffsetXInTab)
{
    draggedBarId_ = barId;
    draggedTabId_ = tabId;
    dragCursorX_.store(cursorX, std::memory_order_relaxed);
    dragOffsetXInTab_.store(dragOffsetXInTab, std::memory_order_relaxed);
    tabBarDirty_ = true;
    setNeedsRedraw();
}

void PlatformDawn::clearDraggedTab()
{
    draggedBarId_ = Uuid { };
    draggedTabId_ = Uuid { };
    tabBarDirty_  = true;
    setNeedsRedraw();
}

void PlatformDawn::setDragCursorX(float cursorX)
{
    dragCursorX_.store(cursorX, std::memory_order_relaxed);
    tabBarDirty_ = true;
    setNeedsRedraw();
}

void PlatformDawn::addPtyPoll(int fd, Terminal *term)
{
    // Reads: dedicated PtyMux thread. The cb runs there. After appending
    // bytes, the cb submits a parse job directly to the worker pool —
    // no main-thread hop. Main only re-enters when the worker fires the
    // Update event, which post()s setNeedsRedraw + dirtyPanes.
    // Writes: lazily registered with eventLoop_ inside Terminal::writeToPTY
    // when bytes first need to block on POLLOUT, and removed by
    // flushWriteQueue when the queue drains. Most panes never have
    // a write registration in steady state.
    term->setEventLoop(eventLoop_.get());
    term->setPtyMux(ptyMux_.get());
    if (renderEngine_) {
        WorkerPool &pool = renderEngine_->workers();
        term->setParseSubmit([&pool](std::function<void()> fn)
                             {
                                 pool.submit(std::move(fn));
                             });
    }
    if (ptyMux_) {
        ptyMux_->add(fd, [term]()
                     {
                         term->readFromFD();
                         term->queueParse();
                     });
    }
    ptyPolls_[fd] = term;
}

void PlatformDawn::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) {
        return;
    }
    // Synchronous remove: blocks until the mux thread has
    // applied the removal and any in-flight callback has
    // returned. Required so the captured `Terminal*` in the
    // callback can't be used after we erase it from the map.
    if (ptyMux_) {
        ptyMux_->remove(fd);
    }
    if (EventLoop *loop = eventLoop_.get()) {
        loop->removeFd(fd); // no-op if write-poll never registered
    }
    ptyPolls_.erase(it);
}

void PlatformDawn::refreshDividers(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) {
        return;
    }
    int divPx = dividerWidth_;
    if (divPx <= 0) {
        return;
    }

    PendingMutations &pending = renderThread_->pending();

    // Clear all divider VBs for this tab's panes
    auto lanes = scriptEngine_.panesInSubtree(subtreeRoot);
    for (Terminal *panePtr : lanes) {
        pending.clearDividerPanes.push_back(panePtr->id());
    }

    if (lanes.size() <= 1) {
        return;
    }
    // When zoomed, tabDividersWithOwnerPanes returns empty naturally
    // (non-zoomed sibling rects aren't in the map, so dividersIn finds
    // nothing to emit).

    auto dividers = scriptEngine_.tabDividersWithOwnerPanes(subtreeRoot, divPx);

    pending.dividersDirty = true;

    const float dr = dividerR_;
    const float dg = dividerG_;
    const float db = dividerB_;
    const float da = dividerA_;

    for (auto &[paneId, dr2] : dividers) {
        PendingMutations::DividerUpdate du;
        du.paneId = paneId;
        du.x      = static_cast<float>(dr2.x);
        du.y      = static_cast<float>(dr2.y);
        du.w      = static_cast<float>(dr2.w);
        du.h      = static_cast<float>(dr2.h);
        du.r      = dr;
        du.g      = dg;
        du.b      = db;
        du.a      = da;
        du.valid  = true;
        pending.dividerUpdates.push_back(du);
    }
}

void PlatformDawn::clearDividers(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) {
        return;
    }
    PendingMutations &pending = renderThread_->pending();
    for (Terminal *panePtr : scriptEngine_.panesInSubtree(subtreeRoot)) {
        pending.clearDividerPanes.push_back(panePtr->id());
    }
    pending.dividersDirty = true;
}

void PlatformDawn::releaseTabTextures(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) {
        return;
    }
    PendingMutations &pending = renderThread_->pending();
    for (Terminal *panePtr : scriptEngine_.panesInSubtree(subtreeRoot)) {
        pending.releasePaneTextures.push_back(panePtr->id());
        pending.dirtyPanes.insert(panePtr->id());
    }
}

void PlatformDawn::notifyPaneFocusChange(Uuid subtreeRoot, Uuid prevId, Uuid newId)
{
    if (subtreeRoot.isNil()) {
        return;
    }
    if (!prevId.isNil()) {
        Terminal *p = scriptEngine_.paneInSubtree(subtreeRoot, prevId);
        if (p) {
            if (!p->focusedPopupId().empty()) {
                scriptEngine_.notifyFocusedPopupChanged(prevId, "");
            }
            p->clearFocusedPopup();
            p->focusEvent(false);
        }
        scriptEngine_.notifyPaneFocusChanged(prevId, false);
    }
    if (!newId.isNil()) {
        Terminal *p = scriptEngine_.paneInSubtree(subtreeRoot, newId);
        if (p) {
            p->focusEvent(true);
        }
        scriptEngine_.notifyPaneFocusChanged(newId, true);
    }
    if (inputController_) {
        inputController_->refreshPointerShape();
    }
}

void PlatformDawn::closeTab(Uuid target)
{
    if (target.isNil()) {
        return;
    }
    LayoutTree &tree       = scriptEngine_.layoutTree();
    const Node *targetNode = tree.node(target);
    if (!targetNode) {
        return;
    }
    // Walk up to the enclosing Stack. Mirrors activateTabByUuid: any
    // direct child of any Stack is closable. Refuses if target isn't
    // anchored under a Stack at all.
    Uuid stackId      = targetNode->parent;
    const Node *stack = nullptr;
    while (!stackId.isNil()) {
        stack = tree.node(stackId);
        if (!stack) {
            return;
        }
        if (std::holds_alternative<StackData>(stack->data)) {
            break;
        }
        stackId = stack->parent;
        stack   = nullptr;
    }
    if (!stack) {
        return;
    }
    const auto *sd = std::get_if<StackData>(&stack->data);
    if (!sd) {
        return;
    }
    int idx = -1;
    for (int i = 0; i < static_cast<int>(sd->children.size()); ++i) {
        if (sd->children[i].id == target) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    // Refuse last child. For the root stack this is "can't close the last
    // top-level tab" (keeps app running). For sub-stacks it would leave a
    // useless empty TabBar/Stack pair — callers that want to dismantle a
    // sub-bar should `mb.layout.removeNode(wrapperContainer)` instead.
    if (sd->children.size() <= 1) {
        return;
    }
    const bool isTopLevel = (stackId == scriptEngine_.layoutRootStack());

    PendingMutations &pending = renderThread_->pending();

    auto tabPanes = scriptEngine_.panesInSubtree(target);
    for (Terminal *panePtr : tabPanes) {
        removePtyPoll(panePtr->masterFD());
        pending.structuralOps.push_back(PendingMutations::DestroyPaneState { panePtr->id() });
        for (const auto &popup : panePtr->popups()) {
            std::string key = popupStateKey(panePtr->id(), popup->popupId());
            pending.releasePopupTextures.push_back(key);
        }
        if (inputController_) {
            inputController_->erasePaneCursorStyle(panePtr->id());
        }
        scriptEngine_.notifyPaneDestroyed(panePtr->id(), panePtr->nodeId());
    }

    // Fire tabDestroyed for any Stack-direct-child closure — top-level OR
    // sub-bar. Payload includes parentStackId so listeners can filter by
    // level if they want.
    scriptEngine_.notifyTabDestroyed(target, stackId);

    std::vector<std::unique_ptr<Terminal>> extractedTerminals;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
        for (Terminal *panePtr : tabPanes) {
            if (!panePtr) {
                continue;
            }
            auto t = scriptEngine_.extractTerminal(panePtr->nodeId());
            if (t) {
                extractedTerminals.push_back(std::move(t));
            }
        }
        // tabIcons_ and lastFocusedInTab_ are evicted automatically by the
        // setOnNodeDestroyed callback wired in Engine ctor.
        tree.removeChild(stackId, target);
        tree.destroyNode(target);
        scriptEngine_.setFocusedTerminalNodeId({ });

        // Activate a surviving sibling within the same Stack (prefer the
        // one before the closed position, else the first remaining).
        int surviving         = (idx > 0) ? (idx - 1) : 0;
        const Node *refreshed = tree.node(stackId);
        const auto *rsd       = refreshed ? std::get_if<StackData>(&refreshed->data) : nullptr;
        if (rsd && surviving < static_cast<int>(rsd->children.size())) {
            Uuid newActive = rsd->children[surviving].id;
            tree.setActiveChild(stackId, newActive);
            auto livePanes = scriptEngine_.panesInSubtree(newActive);
            if (!livePanes.empty()) {
                Uuid remembered = scriptEngine_.rememberedFocusInSubtree(newActive);
                scriptEngine_.setFocusedTerminalNodeId(
                    remembered.isNil() ? livePanes.front()->nodeId() : remembered);
            }
        }
        buildRenderFrameState();
        stamp = renderThread_->completedFrames();
    }
    for (auto &t : extractedTerminals) {
        Terminal *raw = t.get();
        graveyard_.defer(std::move(t), stamp, [raw]()
                         {
                             return !raw->parseInFlight();
                         });
    }

    if (isTopLevel) {
        updateTabBarVisibility();
    }
    if (inputController_) {
        inputController_->refreshPointerShape();
    }
    tabBarDirty_ = true;
    setNeedsRedraw();
    spdlog::info("Closed {} {}", isTopLevel ? "tab" : "sub-tab", idx + 1);
}

void PlatformDawn::terminalExited(Terminal *terminal)
{
    // Called from renderThread_->drainPendingExits() under the render-thread mutex, so
    // the render thread cannot be snapshotting while we mutate live state.
    // Shell-exit path: the PTY is closed, the child has exited. Kill the
    // Terminal synchronously (extract + graveyard + fire event) and let JS
    // decide whether to remove the tree node, close the tab, or quit.
    if (!terminal) {
        return;
    }
    killTerminal(terminal->nodeId());
}

bool PlatformDawn::killTerminal(Uuid nodeId)
{
    // Caller must hold renderThread_->mutex() — this mutates live state
    // the render thread observes through the shadow copy.
    if (nodeId.isNil()) {
        return false;
    }
    Terminal *terminal = scriptEngine_.terminal(nodeId);
    if (!terminal) {
        return false; // already killed or never inserted
    }

    PendingMutations &pending = renderThread_->pending();

    removePtyPoll(terminal->masterFD());
    pending.structuralOps.push_back(PendingMutations::DestroyPaneState { nodeId });
    for (const auto &popup : terminal->popups()) {
        std::string key = popupStateKey(nodeId, popup->popupId());
        pending.releasePopupTextures.push_back(key);
    }
    if (inputController_) {
        inputController_->erasePaneCursorStyle(nodeId);
    }

    std::unique_ptr<Terminal> extracted = scriptEngine_.extractTerminal(nodeId);
    buildRenderFrameState();
    uint64_t stamp = renderThread_->completedFrames();
    if (extracted) {
        Terminal *raw = extracted.get();
        graveyard_.defer(std::move(extracted), stamp, [raw]()
                         {
                             return !raw->parseInFlight();
                         });
    }

    scriptEngine_.notifyTerminalExited(nodeId, nodeId);

    setNeedsRedraw();
    return true;
}

void PlatformDawn::spawnTerminalForPane(Uuid nodeId, Uuid subtreeRoot, const std::string &cwd)
{
    const float charWidth  = charWidth_;
    const float lineHeight = lineHeight_;
    const float padLeft    = padLeft_;
    const float padTop     = padTop_;
    const float padRight   = padRight_;
    const float padBottom  = padBottom_;

    auto cbs = buildTerminalCallbacks(nodeId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal *t)
    {
        renderThread_->enqueueTerminalExit(t);
    };
    pcbs.quit = [this]()
    {
        quit();
    };
    // Filter callbacks are called from the parse worker thread (output)
    // and from writeToOutput (also worker, when the parser writes
    // responses to DA/DSR/OSC52/...). The script engine is main-thread
    // only, so:
    //   * shouldFilter* reads a stable atomic flag via shared_ptr
    //     captured at build time (lifetime extends past pane teardown).
    //   * filter* bounces through runOnMain to invoke QuickJS safely.
    auto outFlag            = scriptEngine_.outputFilterFlag(nodeId);
    auto inFlag             = scriptEngine_.inputFilterFlag(nodeId);
    pcbs.shouldFilterOutput = [outFlag]()
    {
        return outFlag->load(std::memory_order_acquire);
    };
    pcbs.filterOutput = [this, nodeId](std::string &data)
    {
        runOnMain([this, nodeId, &data]()
                  {
                      scriptEngine_.filterPaneOutput(nodeId, data);
                  });
    };
    pcbs.shouldFilterInput = [inFlag]()
    {
        return inFlag->load(std::memory_order_acquire);
    };
    pcbs.filterInput = [this, nodeId](std::string &data)
    {
        runOnMain([this, nodeId, &data]()
                  {
                      scriptEngine_.filterPaneInput(nodeId, data);
                  });
    };
    auto terminal               = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    // Fire the JS "resized" event on any embedded whose cols got updated
    // as a side effect of the parent's resize. Explicit em.resize(rows)
    // from JS fires via the resizeEmbedded platform callback; this
    // handles the orthogonal case where the parent pane itself changed
    // width (split, tab-bar toggle, live-resize).
    terminal->onEmbeddedResized = [this, nodeId](uint64_t lineId, int cols, int rows)
    {
        scriptEngine_.deliverEmbeddedResized(nodeId, lineId, cols, rows);
    };
    // Bridge document-level row eviction to JS `rowEvicted` listeners.
    // The Terminal callback fires on the parse worker under mMutex, so
    // we must hop to the main thread before invoking QuickJS. We do NOT
    // re-acquire mMutex on the main side: lineId is a stable scalar and
    // the engine's notify path only touches JS state (no document
    // access), so there's nothing to lock.
    terminal->onLineIdEvicted = [this, nodeId](uint64_t lineId)
    {
        eventLoop_->post([this, nodeId, lineId]
                         {
                             scriptEngine_.notifyRowEvicted(nodeId, lineId);
                         });
    };
    // Bridge output-capture stop events to JS "stopped" listeners.
    // Fires from whichever thread observed the stop:
    //   - Explicit removeOutputCapture: from the calling thread,
    //     usually main (script .stop()), occasionally main during
    //     unload sweep.
    //   - IO-error auto-stop: from the parse worker (deliverCaptured-
    //     Output runs there during queueParse) or main (during
    //     flushReadBuffer in tests / headless).
    // Either way, hop to the main thread before touching QuickJS.
    terminal->onCaptureStopped =
        [this, nodeId](const std::string &path,
                       Terminal::CaptureStopReason reason,
                       const std::string &errorMessage)
    {
        const char *reasonStr =
            (reason == Terminal::CaptureStopReason::Explicit) ? "explicit"
                                                              : "io-error";
        eventLoop_->post([this, nodeId, path, reasonStr, errorMessage]
                         {
                             scriptEngine_.notifyCaptureStopped(nodeId, path, reasonStr, errorMessage);
                         });
    };
    auto opts = terminalOptions_;
    if (!cwd.empty()) {
        opts.cwd = cwd;
    }

    terminal->applyColorScheme(opts.colors);
    terminal->applyCursorConfig(opts.cursor);
    if (!terminal->init(opts)) {
        spdlog::error("spawnTerminalForPane: failed to init terminal");
        return;
    }

    // Resolve the new pane's pixel rect for the initial PTY size. We need
    // the enclosing top-level tab as the scope for nodeRectInSubtree (it
    // anchors against the tab's allocated content area; sub-tab panes
    // share that area but are nested under it). `findTabSubtreeRootForNode`
    // climbs to the direct child of layoutRootStack regardless of whether
    // the pane sits at top level or inside a sub-bar — exactly what we
    // want here.
    Uuid scopeTab = scriptEngine_.findTabSubtreeRootForNode(nodeId);
    Rect pr       = scopeTab.isNil() ? Rect { } : scriptEngine_.nodeRectInSubtree(scopeTab, nodeId);
    int cols      = (pr.w > 0 && charWidth > 0) ? static_cast<int>((pr.w - padLeft - padRight) / charWidth) : 80;
    int rows      = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;
    cols          = std::max(cols, 1);
    rows          = std::max(rows, 1);

    PendingMutations &pending = renderThread_->pending();
    pending.structuralOps.push_back(PendingMutations::CreatePaneState { nodeId, cols, rows });
    pending.dirtyPanes.insert(nodeId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    int masterFD      = terminal->masterFD();
    Terminal *termPtr = terminal.get();

    if (!scopeTab.isNil()) {
        terminal->setNodeId(nodeId);
        scriptEngine_.insertTerminal(nodeId, std::move(terminal));
    }
    addPtyPoll(masterFD, termPtr);

    scriptEngine_.notifyPaneCreated(subtreeRoot, nodeId);
}

void PlatformDawn::resizeAllPanesInTab(Uuid subtreeRoot)
{
    if (subtreeRoot.isNil()) {
        return;
    }

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

    PendingMutations &pending = renderThread_->pending();

    for (Terminal *pane : scriptEngine_.panesInSubtree(subtreeRoot)) {
        pane->resizeToRect(charWidth, lineHeight, padLeft, padTop, padRight, padBottom);

        int cols = std::max(pane->width(), 1);
        int rows = std::max(pane->height(), 1);
        Uuid id  = pane->nodeId();

        pending.structuralOps.push_back(
            PendingMutations::ResizePaneState { id, cols, rows });
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
    if (sub.isNil()) {
        return std::nullopt;
    }
    return sub;
}

Uuid PlatformDawn::createEmptyTab()
{
    const bool headless = isHeadless();
    if (!window_ && !headless) {
        return { };
    }

    Uuid subRoot = scriptEngine_.createTabSubtree();
    scriptEngine_.setDividerPixels(dividerWidth_);

    attachLayoutSubtree(subRoot, /*activate=*/false);

    updateTabBarVisibility();
    tabBarDirty_ = true;
    setNeedsRedraw();

    scriptEngine_.notifyTabCreated(subRoot, scriptEngine_.layoutRootStack());
    return subRoot;
}

void PlatformDawn::activateTabByUuid(Uuid target)
{
    if (target.isNil()) {
        return;
    }
    LayoutTree &tree       = scriptEngine_.layoutTree();
    const Node *targetNode = tree.node(target);
    if (!targetNode) {
        return;
    }

    // Walk up to the enclosing Stack. `target` must be a direct child of
    // some Stack; otherwise activation is meaningless.
    Uuid stackId      = targetNode->parent;
    const Node *stack = nullptr;
    while (!stackId.isNil()) {
        stack = tree.node(stackId);
        if (!stack) {
            return;
        }
        if (std::holds_alternative<StackData>(stack->data)) {
            break;
        }
        stackId = stack->parent;
        stack   = nullptr;
    }
    if (!stack) {
        return;
    }
    const auto *sd = std::get_if<StackData>(&stack->data);
    if (!sd) {
        return;
    }
    bool isDirectChild = false;
    for (const auto &slot : sd->children) {
        if (slot.id == target) {
            isDirectChild = true;
            break;
        }
    }
    if (!isDirectChild) {
        return;
    }
    Uuid prevChild = sd->activeChild;

    // Per-subtree GPU teardown for the previously-active sibling so its
    // textures + dividers don't linger off-screen.
    if (!prevChild.isNil() && prevChild != target) {
        clearDividers(prevChild);
        releaseTabTextures(prevChild);
    }

    tree.setActiveChild(stackId, target);
    refreshDividers(target);

    // Focus selection + notification is the caller's job (default-ui's
    // activateTab handler calls mb.layout.focusPane, which fires
    // notifyPaneFocusChange + updates the window title).
    tabBarDirty_ = true;
    setNeedsRedraw();
}

bool PlatformDawn::createTerminalInContainer(Uuid parentContainerNodeId,
                                             const std::string &cwdIn,
                                             Uuid *outNodeId)
{
    LayoutTree &tree = scriptEngine_.layoutTree();

    auto tab = findTabForNode(parentContainerNodeId);
    if (!tab) {
        return false;
    }

    Uuid attachParent = parentContainerNodeId;
    if (const Node *n = tree.node(attachParent)) {
        if (auto *sd = std::get_if<StackData>(&n->data)) {
            if (!sd->activeChild.isNil()) {
                attachParent = sd->activeChild;
            }
        }
    }

    Uuid newNodeId = scriptEngine_.allocatePaneNode();
    if (newNodeId.isNil()) {
        return false;
    }
    if (!tree.appendChild(attachParent, ChildSlot { newNodeId, /*stretch=*/1 })) {
        return false;
    }

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
            if (Terminal *fp = scriptEngine_.terminal(focusedNode)) {
                cwd = paneProcessCWD(fp);
            }
        }
    }

    spawnTerminalForPane(newNodeId, *tab, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) {
        *outNodeId = newNodeId;
    }
    return true;
}

bool PlatformDawn::splitPaneByNodeId(Uuid existingPaneNodeId, SplitDir dir,
                                     float ratio, bool newIsFirst,
                                     const std::string &cwdIn,
                                     Uuid *outNodeId)
{
    (void)ratio;
    auto tab = findTabForNode(existingPaneNodeId);
    if (!tab) {
        return false;
    }

    Uuid newNodeId = scriptEngine_.allocatePaneNode();
    if (newNodeId.isNil()) {
        return false;
    }
    if (!scriptEngine_.splitByNodeId(existingPaneNodeId, dir, newNodeId, newIsFirst)) {
        return false;
    }

    const int cellW = std::max(1, static_cast<int>(std::round(charWidth_)));
    const int cellH = std::max(1, static_cast<int>(std::round(lineHeight_)));
    scriptEngine_.computeTabRects(*tab, fbWidth_, fbHeight_, cellW, cellH);

    // Caller-supplied cwd wins; fall back to paneProcessCWD so user
    // scripts that call mb.layout.splitPane(...) without opts still get
    // automatic inheritance.
    std::string cwd = cwdIn;
    if (cwd.empty()) {
        if (Terminal *fp = scriptEngine_.paneInSubtree(*tab, existingPaneNodeId)) {
            cwd = paneProcessCWD(fp);
        }
    }

    spawnTerminalForPane(newNodeId, *tab, cwd);
    resizeAllPanesInTab(*tab);

    if (outNodeId) {
        *outNodeId = newNodeId;
    }
    return true;
}

bool PlatformDawn::focusPaneById(Uuid nodeId)
{
    auto tab = findTabForPane(nodeId);
    if (!tab) {
        return false;
    }
    Uuid prev = scriptEngine_.focusedPaneInSubtree(*tab);
    scriptEngine_.setFocusedTerminalNodeId(nodeId);
    notifyPaneFocusChange(*tab, prev, nodeId);
    // Tab bar reads title/icon live off the focused pane, so just mark dirty.
    tabBarDirty_ = true;
    if (*tab == scriptEngine_.activeTabSubtreeRoot()) {
        updateWindowTitle();
    }
    setNeedsRedraw();
    return true;
}

bool PlatformDawn::removeNode(Uuid nodeId)
{
    auto tab = findTabForNode(nodeId);
    if (!tab) {
        return false;
    }
    if (nodeId == *tab) {
        return false;
    }

    bool removed = false;
    {
        std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
        removed = scriptEngine_.removeNodeSubtree(*tab, nodeId);
        if (removed) {
            buildRenderFrameState();
        }
    }
    if (!removed) {
        return false;
    }

    auto livePanes = scriptEngine_.panesInSubtree(*tab);
    if (livePanes.empty()) {
        setNeedsRedraw();
        return true;
    }

    if (scriptEngine_.focusedPaneInSubtree(*tab).isNil()) {
        scriptEngine_.setFocusedTerminalNodeId(livePanes.front()->nodeId());
    }

    resizeAllPanesInTab(*tab);
    notifyPaneFocusChange(*tab, Uuid { }, scriptEngine_.focusedPaneInSubtree(*tab));
    tabBarDirty_ = true;
    if (*tab == scriptEngine_.activeTabSubtreeRoot()) {
        updateWindowTitle();
    }
    setNeedsRedraw();
    return true;
}

// ---------------------------------------------------------------------------
// Terminal callbacks builder
// ---------------------------------------------------------------------------

TerminalCallbacks PlatformDawn::buildTerminalCallbacks(Uuid paneId)
{
    TerminalCallbacks cbs;

    cbs.event = [this, paneId](TerminalEmulator *, int ev, void *payload)
    {
        switch (static_cast<TerminalEmulator::Event>(ev)) {
            case TerminalEmulator::Update:
            case TerminalEmulator::ScrollbackChanged:
                setNeedsRedraw();
                eventLoop_->post([this, paneId]
                                 {
                                     renderThread_->pending().dirtyPanes.insert(paneId);
                                 });
                break;
            case TerminalEmulator::VisibleBell:
                break;
            case TerminalEmulator::CommandComplete:
                if (payload) {
                    const auto *rec                         = static_cast<const TerminalEmulator::CommandRecord *>(payload);
                    TerminalEmulator::CommandRecord recCopy = *rec;
                    eventLoop_->post([this, paneId, recCopy = std::move(recCopy)]
                                     {
                                         TerminalEmulator *te = scriptEngine_.terminal(paneId);
                                         if (!te) {
                                             return;
                                         }
                                         Script::CommandInfo info;
                                         {
                                             // Lock for the document line-id resolution; the
                                             // worker is concurrently mutating the document.
                                             std::lock_guard<std::recursive_mutex> _lk(te->mutex());
                                             const auto &doc = te->document();
                                             info            = Script::CommandInfo {
                                                 recCopy.id,
                                                 recCopy.cwd,
                                                 recCopy.exitCode,
                                                 recCopy.startMs,
                                                 recCopy.endMs,
                                                 recCopy.promptStartLineId,
                                                 recCopy.commandStartLineId,
                                                 recCopy.outputStartLineId,
                                                 recCopy.outputEndLineId,
                                                 doc.firstAbsOfLine(recCopy.promptStartLineId),
                                                 recCopy.promptStartCol,
                                                 doc.firstAbsOfLine(recCopy.commandStartLineId),
                                                 recCopy.commandStartCol,
                                                 doc.firstAbsOfLine(recCopy.outputStartLineId),
                                                 recCopy.outputStartCol,
                                                 doc.lastAbsOfLine(recCopy.outputEndLineId),
                                                 recCopy.outputEndCol
                                             };
                                         }
                                         scriptEngine_.notifyCommandComplete(paneId, info);
                                     });
                }
                break;
            case TerminalEmulator::CommandSelectionChanged:
                eventLoop_->post([this, paneId]
                                 {
                                     Terminal *te = scriptEngine_.terminal(paneId);
                                     if (!te) {
                                         return;
                                     }
                                     std::optional<uint64_t> id;
                                     {
                                         std::lock_guard<std::recursive_mutex> _lk(te->mutex());
                                         id = te->selectedCommandId();
                                     }
                                     scriptEngine_.notifyCommandSelectionChanged(paneId, id);
                                 });
                break;
            case TerminalEmulator::AltScreenChanged:
                eventLoop_->post([this, paneId]
                                 {
                                     Terminal *te = scriptEngine_.terminal(paneId);
                                     if (!te) {
                                         return;
                                     }
                                     scriptEngine_.notifyAltScreenChanged(paneId, te->usingAltScreen());
                                 });
                break;
        }
    };

    if (!isHeadless()) {
        cbs.copyToClipboard = [this](const std::string &text, ClipboardTarget target)
        {
            eventLoop_->post([this, text, target]
                             {
                                 if (!window_) {
                                     return;
                                 }
                                 if (target == ClipboardTarget::Primary) {
                                     window_->setPrimarySelection(text);
                                 } else {
                                     window_->setClipboard(text);
                                 }
                             });
        };
        // OSC 52 c=? query runs on a parse worker. We post the actual
        // selection request to the main thread, fulfilling a promise on
        // completion — main thread keeps running its event loop normally
        // and never busy-polls. Worker blocks on the future. requestSelection
        // is async (registers a callback that fires when SELECTION_NOTIFY
        // arrives or the 5 s sweep deadline expires), so this never
        // contends with a concurrent middle-click paste.
        cbs.pasteFromClipboard = [this](ClipboardTarget target) -> std::string
        {
            auto src = (target == ClipboardTarget::Primary)
                ? Window::SelectionSource::Primary
                : Window::SelectionSource::Clipboard;
            auto p   = std::make_shared<std::promise<std::string>>();
            auto fut = p->get_future();
            eventLoop_->post([this, src, p]() mutable
                             {
                                 if (!window_) {
                                     p->set_value({ });
                                     return;
                                 }
                                 window_->requestSelection(src,
                                                           [p](std::optional<std::string> text) mutable
                                                           {
                                                               p->set_value(text.value_or(std::string { }));
                                                           });
                             });
            return fut.get();
        };
    } else {
        cbs.copyToClipboard = [](const std::string &, ClipboardTarget)
        {
        };
        cbs.pasteFromClipboard = [](ClipboardTarget) -> std::string
        {
            return { };
        };
    }

    // Pull-model: title/icon live on the emulator's XTWINOPS stack. These
    // callbacks only need to dirty the tab bar + refresh the window title
    // so the render thread re-reads. std::nullopt means "stack went empty"
    // (pop-to-empty); Some("") is an explicit OSC 2 "" — both treated the
    // same here since the pull side handles resolution.
    cbs.onTitleChanged = [this, paneId](std::optional<std::string>)
    {
        eventLoop_->post([this, paneId]
                         {
                             auto tab = findTabForPane(paneId);
                             if (!tab) {
                                 return;
                             }
                             // Sub-bar tab labels read this pane's title too
                             // (when it's the remembered focus for the
                             // sub-stack child containing it). Cheaper to
                             // always mark the bar dirty — the renderer's
                             // per-bar FNV-1a hash skips no-op redraws.
                             tabBarDirty_ = true;
                             setNeedsRedraw();
                             // Window title still gates on "this pane is the
                             // top-level representative AND that tab is
                             // active" — that's the title shown by the OS.
                             if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId && *tab == scriptEngine_.activeTabSubtreeRoot()) {
                                 updateWindowTitle();
                             }
                         });
    };

    cbs.onIconChanged = [this, paneId](std::optional<std::string>)
    {
        eventLoop_->post([this, paneId]
                         {
                             auto tab = findTabForPane(paneId);
                             if (!tab) {
                                 return;
                             }
                             tabBarDirty_ = true;
                             setNeedsRedraw();
                             if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId && *tab == scriptEngine_.activeTabSubtreeRoot()) {
                                 updateWindowTitle();
                             }
                         });
    };

    cbs.onProgressChanged = [this, paneId](int state, int pct)
    {
        eventLoop_->post([this, paneId, state, pct]
                         {
                             auto tab = findTabForPane(paneId);
                             if (!tab) {
                                 return;
                             }
                             if (Terminal *p = scriptEngine_.paneInSubtree(*tab, paneId)) {
                                 p->setProgress(state, pct);
                                 tabBarDirty_ = true;
                                 setNeedsRedraw();
                             }
                         });
    };

    // charWidth_ / lineHeight_ are floats written from the main thread
    // during font setup / config reload and read from the parse worker
    // (KittyGraphics, OSC 14/4 reports). float reads are atomic-aligned
    // on every platform we support and the values change rarely; reading
    // a slightly stale value is harmless for the OSC reply path.
    cbs.cellPixelWidth = [this]() -> float
    {
        return charWidth_;
    };
    cbs.cellPixelHeight = [this]() -> float
    {
        return lineHeight_;
    };
    // Mode 2031 / DSR-997 dark-mode query. The parser worker calls this
    // synchronously from inside injectData while holding Terminal::mutex().
    // The previous runOnMain bounce deadlocked against the render thread
    // (which also takes Terminal::mutex() via forEachEmbedded). The actual
    // OS state is queried only by main (config reload / appearance observer)
    // and stashed in cachedIsDarkMode_; the parser reads the atomic directly.
    cbs.isDarkMode = [this]()
    {
        return cachedIsDarkMode();
    };

    cbs.onCWDChanged = [this, paneId](const std::string &dir)
    {
        eventLoop_->post([this, paneId, dir]
                         {
                             auto tab = findTabForPane(paneId);
                             if (!tab) {
                                 return;
                             }
                             if (Terminal *p = scriptEngine_.paneInSubtree(*tab, paneId)) {
                                 p->setCWD(dir);
                             }
                         });
    };

    if (isHeadless()) {
        cbs.onDesktopNotification = [](const TerminalCallbacks::DesktopNotification &)
        {
        };
        cbs.onCloseNotification = [](const std::string &)
        {
        };
        cbs.onQueryAliveNotifications = [](const std::string &)
        {
        };
    } else {
        // sourceTag = pane uuid stringified. clientId = OSC i= (may be
        // empty for un-tracked notifications, in which case the platform
        // skips replaces_id bookkeeping).
        cbs.onDesktopNotification = [this, paneId](const TerminalCallbacks::DesktopNotification &n)
        {
            std::string sourceTag = paneId.toString();
            // Capture the notification by value (struct holds vectors/strings).
            eventLoop_->post([this, paneId, sourceTag, n]() mutable
                             {
                                 std::function<void(const std::string &)> onClosed;
                                 if (n.closeResponseRequested && !n.id.empty()) {
                                     std::string clientId = n.id;
                                     onClosed             = [this, paneId, clientId](const std::string &reason)
                                     {
                                         std::string osc = "\x1b]99;i=" + clientId + ":p=close;" + reason + "\a";
                                         if (Terminal *term = scriptEngine_.terminal(paneId)) {
                                             term->writeText(osc);
                                         }
                                     };
                                 }
                                 // onActivated fires when the user clicks the body or a button.
                                 // buttonId is empty for the body click, "1".."N" for buttons.
                                 std::function<void(const std::string &)> onActivated;
                                 if (n.actionFocus || n.actionReport) {
                                     bool focusReq        = n.actionFocus;
                                     bool reportReq       = n.actionReport;
                                     std::string clientId = n.id;
                                     onActivated          = [this, paneId, focusReq, reportReq, clientId](const std::string &buttonId)
                                     {
                                         if (focusReq) {
                                             // Activate the tab that owns the pane (if not
                                             // already active), focus the pane within it,
                                             // then raise the window. Order matters: pane
                                             // focus updates the per-tab "remembered focus"
                                             // state, which the window's focus handler reads
                                             // on activation.
                                             if (auto tab = findTabForPane(paneId)) {
                                                 if (*tab != scriptEngine_.activeTabSubtreeRoot()) {
                                                     activateTabByUuid(*tab);
                                                 }
                                                 focusPaneById(paneId);
                                             }
                                             if (window_) {
                                                 window_->raise();
                                             }
                                         }
                                         if (reportReq && !clientId.empty()) {
                                             std::string osc = "\x1b]99;i=" + clientId + ";" + buttonId + "\a";
                                             if (Terminal *term = scriptEngine_.terminal(paneId)) {
                                                 term->writeText(osc);
                                             }
                                         }
                                     };
                                 }
                                 platformSendNotification(sourceTag, n.id, n.title, n.body, n.urgency, n.closeResponseRequested, std::move(onClosed), n.buttons, std::move(onActivated), n.onlyWhen);
                             });
        };

        cbs.onCloseNotification = [this, paneId](const std::string &clientId)
        {
            std::string sourceTag = paneId.toString();
            eventLoop_->post([sourceTag, clientId]
                             {
                                 platformCloseNotification(sourceTag, clientId);
                             });
        };

        cbs.onQueryAliveNotifications = [this, paneId](const std::string &responderId)
        {
            std::string sourceTag = paneId.toString();
            eventLoop_->post([this, paneId, sourceTag, responderId]
                             {
                                 std::vector<std::string> alive = platformActiveNotifications(sourceTag);
                                 std::string csv;
                                 for (size_t i = 0; i < alive.size(); ++i) {
                                     if (i) {
                                         csv.push_back(',');
                                     }
                                     csv.append(alive[i]);
                                 }
                                 std::string osc = "\x1b]99;i=" + responderId + ":p=alive;" + csv + "\a";
                                 if (Terminal *term = scriptEngine_.terminal(paneId)) {
                                     term->writeText(osc);
                                 }
                             });
        };
    }

    cbs.onOSC = [this, paneId](int oscNum, std::string_view payload)
    {
        std::string payloadCopy(payload);
        eventLoop_->post([this, paneId, oscNum, payloadCopy = std::move(payloadCopy)]
                         {
                             scriptEngine_.notifyOSC(paneId, oscNum, payloadCopy);
                         });
    };

    // DCS+q (XTGETTCAP) for caps not in the built-in table. The parser worker
    // calls this synchronously from inside applyActions while holding
    // Terminal::mutex(). Bouncing through runOnMain here used to deadlock
    // against the render thread (which also takes Terminal::mutex() via
    // forEachEmbedded in buildRenderFrameState). The custom-tcap registry is
    // pure data; lookupCustomTcap is now thread-safe (shared_mutex inside
    // ScriptEngine), so we call it directly from the parser thread.
    cbs.customTcapLookup = [this](const std::string &name) -> std::optional<std::string>
    {
        return scriptEngine_.lookupCustomTcap(name);
    };

    cbs.onMouseCursorShape = [this, paneId](const std::string &shape)
    {
        eventLoop_->post([this, paneId, shape]
                         {
                             if (!inputController_) {
                                 return;
                             }
                             if (shape.empty()) {
                                 inputController_->erasePaneCursorStyle(paneId);
                             } else {
                                 inputController_->setPaneCursorStyle(paneId,
                                                                      InputController::pointerShapeNameToCursorStyle(shape));
                             }
                             if (!window_ || isHeadless()) {
                                 return;
                             }
                             auto tab = activeTab();
                             if (!tab || scriptEngine_.focusedPaneInSubtree(*tab) != paneId) {
                                 return;
                             }
                             window_->setCursorStyle(shape.empty()
                                                         ? Window::CursorStyle::IBeam
                                                         : inputController_->paneCursorStyle(paneId));
                         });
    };

    cbs.onForegroundProcessChanged = [this, paneId](const std::string &proc)
    {
        eventLoop_->post([this, paneId, proc]
                         {
                             scriptEngine_.notifyForegroundProcessChanged(paneId, proc);
                             auto tab = findTabForPane(paneId);
                             if (!tab) {
                                 return;
                             }
                             // Sub-bar tab labels also fall back to fg
                             // process when their representative pane has
                             // no OSC title. Unconditionally dirty the
                             // bars; the renderer's per-bar hash skips
                             // redundant work.
                             tabBarDirty_ = true;
                             setNeedsRedraw();
                             if (scriptEngine_.rememberedFocusInSubtree(*tab) == paneId && *tab == scriptEngine_.activeTabSubtreeRoot()) {
                                 updateWindowTitle();
                             }
                         });
    };

    return cbs;
}
