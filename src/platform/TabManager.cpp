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

// collectFirstPaneDividers used to walk Layout's legacy binary-split tree.
// Now Layout::dividersWithOwnerPanes returns the same (firstLeafPaneId, rect)
// pairs directly from the LayoutTree walk — see Layout.cpp.


Terminal* TabManager::activeTerm()
{
    Tab* tab = activeTab();
    if (!tab) return nullptr;
    if (tab->hasOverlay()) return tab->topOverlay();
    Terminal* pane = tab->layout()->focusedPane();
    return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
}


void TabManager::notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn)
{
    for (auto& tab : tabs_) {
        for (auto& panePtr : tab->layout()->panes()) {
            fn(panePtr.get());
        }
    }
}


Tab* TabManager::findTabForPane(int paneId, int* outTabIdx)
{
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        if (tabs_[i]->layout()->pane(paneId)) {
            if (outTabIdx) *outTabIdx = i;
            return tabs_[i].get();
        }
    }
    return nullptr;
}


void TabManager::setActiveTabIdx(int idx)
{
    activeTabIdx_ = idx;
    // Keep the shared tree's root Stack activeChild in sync with the visible
    // tab so JS observers see a consistent view.
    if (host_.scriptEngine &&
        idx >= 0 && idx < static_cast<int>(tabs_.size())) {
        Uuid sub = tabs_[idx]->layout()->subtreeRoot();
        if (!sub.isNil()) {
            host_.scriptEngine->layoutTree().setActiveChild(
                host_.scriptEngine->layoutRootStack(), sub);
        }
    }
}

void TabManager::attachLayoutSubtree(Tab* tab, bool activate)
{
    if (!tab || !host_.scriptEngine) return;
    LayoutTree& tree = host_.scriptEngine->layoutTree();
    Uuid rootStack = host_.scriptEngine->layoutRootStack();
    Uuid sub = tab->layout()->subtreeRoot();
    if (sub.isNil()) return;
    tree.appendChild(rootStack, ChildSlot{sub, /*stretch=*/1});
    if (activate) tree.setActiveChild(rootStack, sub);
}

void TabManager::addInitialTab(std::unique_ptr<Tab> tab)
{
    activeTabIdx_ = static_cast<int>(tabs_.size());
    Tab* raw = tab.get();
    tabs_.push_back(std::move(tab));
    attachLayoutSubtree(raw, /*activate=*/true);
}


void TabManager::updateWindowTitle()
{
    if (host_.headless && host_.headless()) return;
    Tab* t = activeTab();
    if (!t) return;
    Terminal* fp = t->layout()->focusedPane();
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


void TabManager::refreshDividers(Tab* tab)
{
    if (!tab) return;
    int divPx = host_.dividerWidth ? host_.dividerWidth() : 0;
    if (divPx <= 0) return;
    Layout* layout = tab->layout();

    // Clear all divider VBs for this tab's panes
    for (auto& panePtr : layout->panes())
        host_.pending->clearDividerPanes.push_back(panePtr->id());

    if (layout->panes().size() <= 1 || layout->isZoomed()) return;

    // Collect (paneId, dividerRect) for each split boundary in the tree.
    std::vector<std::pair<int, PaneRect>> dividers = layout->dividersWithOwnerPanes(divPx);

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


void TabManager::clearDividers(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes())
        host_.pending->clearDividerPanes.push_back(panePtr->id());
    host_.pending->dividersDirty = true;
}


void TabManager::releaseTabTextures(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes()) {
        host_.pending->releasePaneTextures.push_back(panePtr->id());
        host_.pending->dirtyPanes.insert(panePtr->id());
    }
}


void TabManager::notifyPaneFocusChange(Tab* tab, int prevId, int newId)
{
    if (!tab) return;
    if (prevId >= 0) {
        Terminal* p = tab->layout()->pane(prevId);
        if (p) {
            if (!p->focusedPopupId().empty() && host_.scriptEngine)
                host_.scriptEngine->notifyFocusedPopupChanged(prevId, "");
            p->clearFocusedPopup();
            p->focusEvent(false);
        }
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(prevId, false);
    }
    if (newId >= 0) {
        Terminal* p = tab->layout()->pane(newId);
        if (p) p->focusEvent(true);
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(newId, true);
    }
    if (host_.inputController) host_.inputController->refreshPointerShape();
}


void TabManager::updateTabTitleFromFocusedPane(int tabIdx)
{
    if (tabIdx < 0 || tabIdx >= static_cast<int>(tabs_.size())) return;
    Tab* tab = tabs_[tabIdx].get();
    Terminal* fp = tab->layout()->focusedPane();
    if (!fp) return;

    const std::string& title = fp->title();
    const std::string& icon  = fp->icon();
    tab->setTitle(title);
    if (!icon.empty()) tab->setIcon(icon);
    if (tabIdx == activeTabIdx_ && !title.empty())
        updateWindowTitle();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}


void TabManager::createTab()
{
    // Device/window readiness checks live on the caller (PlatformDawn)
    // in createTerminal — by the time createTab runs, the platform is
    // initialized. Mirror the early guards: if there's no event loop or
    // no window (in windowed mode) we can't spawn.
    const bool headless = host_.headless && host_.headless();
    Window* win = host_.window ? host_.window() : nullptr;
    if (!win && !headless) return;

    const float charWidth  = host_.charWidth ? host_.charWidth() : 0.0f;
    const float lineHeight = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const float padLeft    = host_.padLeft ? host_.padLeft() : 0.0f;
    const float padTop     = host_.padTop ? host_.padTop() : 0.0f;
    const float padRight   = host_.padRight ? host_.padRight() : 0.0f;
    const float padBottom  = host_.padBottom ? host_.padBottom() : 0.0f;
    const uint32_t fbWidth  = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbHeight = host_.fbHeight ? host_.fbHeight() : 0;
    const int divW = host_.dividerWidth ? host_.dividerWidth() : 0;

    // Inherit CWD from the focused pane of the active tab
    auto opts = terminalOptions_;
    if (Tab* at = activeTab()) {
        if (Terminal* fp = at->layout()->focusedPane()) {
            std::string cwd = paneProcessCWD(fp);
            if (!cwd.empty()) opts.cwd = cwd;
        }
    }

    // Layout's subtree lives in the shared LayoutTree on the Engine, so JS
    // bindings and native code see the same structural state.
    auto layout = std::make_unique<Layout>(host_.scriptEngine
                                           ? &host_.scriptEngine->layoutTree()
                                           : nullptr);
    layout->setDividerPixels(divW);

    // Allocate an ID and tree node — no Terminal created yet.
    int paneId = layout->createPane();
    layout->setFocusedPane(paneId);

    // Build the Terminal with callbacks that capture the pane ID.
    auto cbs = host_.buildTerminalCallbacks(paneId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) {
        if (host_.queueTerminalExit) host_.queueTerminalExit(t);
    };
    pcbs.quit = [this]() { if (host_.quit) host_.quit(); };
    Script::Engine* scriptEngine = host_.scriptEngine;
    pcbs.shouldFilterOutput = [scriptEngine, paneId]() {
        return scriptEngine->hasPaneOutputFilters(paneId);
    };
    pcbs.filterOutput = [scriptEngine, paneId](std::string& data) {
        scriptEngine->filterPaneOutput(paneId, data);
    };
    pcbs.shouldFilterInput = [scriptEngine, paneId]() {
        return scriptEngine->hasPaneInputFilters(paneId);
    };
    pcbs.filterInput = [scriptEngine, paneId](std::string& data) {
        scriptEngine->filterPaneInput(paneId, data);
    };

    auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    terminal->applyColorScheme(opts.colors);
    terminal->applyCursorConfig(opts.cursor);
    if (!terminal->init(opts)) {
        spdlog::error("createTab: failed to init terminal");
        return;
    }

    // Apply tab bar height to the new layout
    const bool barVisible = host_.tabBarVisible ? host_.tabBarVisible() : false;
    const float tbLine = host_.tabBarLineHeight ? host_.tabBarLineHeight() : 0.0f;
    if (barVisible && tbLine > 0.0f) {
        std::string pos = host_.tabBarPosition ? host_.tabBarPosition() : "top";
        layout->setTabBar(static_cast<int>(std::ceil(tbLine)), pos);
    }
    layout->computeRects(fbWidth, fbHeight);

    const PaneRect& pr = layout->nodeRect(paneId);
    int cols = (pr.w > 0 && charWidth > 0) ? static_cast<int>((pr.w - padLeft - padRight) / charWidth) : 80;
    int rows = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;

    int tabIdx = static_cast<int>(tabs_.size());
    host_.pending->structuralOps.push_back(PendingMutations::CreatePaneState{paneId, cols, rows});
    host_.pending->dirtyPanes.insert(paneId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();
    layout->insertTerminal(paneId, std::move(terminal));
    addPtyPoll(masterFD, termPtr);

    activeTabIdx_ = tabIdx;
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(termPtr->title());
    Tab* tabRaw = tab.get();
    tabs_.push_back(std::move(tab));
    attachLayoutSubtree(tabRaw, /*activate=*/true);

    if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
    updateWindowTitle();
    if (host_.inputController) host_.inputController->refreshPointerShape();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();

    if (host_.scriptEngine) {
        host_.scriptEngine->notifyTabCreated(tabIdx);
        host_.scriptEngine->notifyPaneCreated(tabIdx, paneId);
    }

    spdlog::info("Created tab {}", tabIdx + 1);
}


void TabManager::closeTab(int idx)
{
    if (tabs_.empty() || idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    if (tabs_.size() == 1) return; // can't close the last tab

    // Stop PTY polls for all terminals in this tab. Queue render state
    // cleanup and fire script notifications before the Tab leaves its
    // slot — scripts and structural-op consumers observe the pre-destroy
    // state, while the actual C++ destruction is deferred via the
    // graveyard below.
    Tab* tab = tabs_[idx].get();
    for (auto& panePtr : tab->layout()->panes()) {
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

    if (tab->hasOverlay()) {
        if (host_.scriptEngine) host_.scriptEngine->notifyOverlayDestroyed(idx);
        host_.pending->structuralOps.push_back(PendingMutations::DestroyOverlayState{});
    }
    if (host_.scriptEngine)
        host_.scriptEngine->notifyTabDestroyed(idx, tab->layout()->subtreeRoot());

    // Extract the Tab under the render-thread mutex, rebuild the shadow
    // copy to reflect the new active tab, and stage the extracted Tab
    // into the graveyard. The render thread may still hold raw pointers
    // to any Terminal inside this tab from an in-flight frame; the stamp
    // waits for that frame to end before destructors run.
    std::unique_ptr<Tab> extracted;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::recursive_mutex> plk(*host_.platformMutex);
        extracted = std::move(tabs_[idx]);
        tabs_.erase(tabs_.begin() + idx);
        if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
            activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
        if (host_.buildRenderFrameState) host_.buildRenderFrameState();
        stamp = host_.completedFrames ? host_.completedFrames() : 0;
    }
    if (extracted && host_.graveyard)
        host_.graveyard->defer(std::move(extracted), stamp);

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
    for (int tabIdx = 0; tabIdx < static_cast<int>(tabs_.size()); ++tabIdx) {
        Tab* tab = tabs_[tabIdx].get();
        for (auto& panePtr : tab->layout()->panes()) {
            if (panePtr.get() != terminal) continue;

            int paneId = panePtr->id();
            if (host_.scriptEngine)
                host_.scriptEngine->notifyTerminalExited(paneId, panePtr->nodeId());

            removePtyPoll(terminal->masterFD());

            host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{paneId});
            for (const auto& popup : panePtr->popups()) {
                std::string key = popupStateKey(paneId, popup->popupId());
                host_.pending->releasePopupTextures.push_back(key);
            }
            if (host_.inputController) host_.inputController->erasePaneCursorStyle(paneId);

            if (host_.scriptEngine)
                host_.scriptEngine->notifyPaneDestroyed(paneId, panePtr->nodeId());

            if (tab->layout()->panes().size() <= 1) {
                // Last pane in the tab — close the whole tab. Extract and
                // stage into the graveyard so the render thread's
                // in-flight frame can finish using its frameState_ pointers.
                if (tab->hasOverlay() && host_.scriptEngine)
                    host_.scriptEngine->notifyOverlayDestroyed(tabIdx);
                if (host_.scriptEngine)
                    host_.scriptEngine->notifyTabDestroyed(tabIdx,
                                                           tab->layout()->subtreeRoot());

                std::unique_ptr<Tab> extractedTab = std::move(tabs_[tabIdx]);
                tabs_.erase(tabs_.begin() + tabIdx);
                if (tabs_.empty()) {
                    if (host_.graveyard)
                        host_.graveyard->defer(std::move(extractedTab),
                            host_.completedFrames ? host_.completedFrames() : 0);
                    if (host_.quit) host_.quit();
                    return;
                }
                if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
                    activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
                // Refresh the shadow copy to reflect the new active tab
                // before we release the mutex.
                if (host_.buildRenderFrameState) host_.buildRenderFrameState();
                uint64_t stamp = host_.completedFrames ? host_.completedFrames() : 0;
                if (host_.graveyard)
                    host_.graveyard->defer(std::move(extractedTab), stamp);

                if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
                updateWindowTitle();
                if (host_.inputController) host_.inputController->refreshPointerShape();
                if (host_.markTabBarDirty) host_.markTabBarDirty();
            } else {
                std::unique_ptr<Terminal> extractedPane = tab->layout()->extractPane(paneId);
                if (extractedPane) {
                    if (host_.buildRenderFrameState) host_.buildRenderFrameState();
                    uint64_t stamp = host_.completedFrames ? host_.completedFrames() : 0;
                    if (host_.graveyard)
                        host_.graveyard->defer(std::move(extractedPane), stamp);
                }
                resizeAllPanesInTab(tab);
                notifyPaneFocusChange(tab, -1, tab->layout()->focusedPaneId());
                updateTabTitleFromFocusedPane(tabIdx);
            }

            if (host_.setNeedsRedraw) host_.setNeedsRedraw();
            return;
        }
    }
    // Fallback: terminal not found in any pane
    if (host_.quit) host_.quit();
}


void TabManager::spawnTerminalForPane(int paneId, int tabIdx, const std::string& cwd)
{
    const float charWidth  = host_.charWidth ? host_.charWidth() : 0.0f;
    const float lineHeight = host_.lineHeight ? host_.lineHeight() : 0.0f;
    const float padLeft    = host_.padLeft ? host_.padLeft() : 0.0f;
    const float padTop     = host_.padTop ? host_.padTop() : 0.0f;
    const float padRight   = host_.padRight ? host_.padRight() : 0.0f;
    const float padBottom  = host_.padBottom ? host_.padBottom() : 0.0f;

    auto cbs = host_.buildTerminalCallbacks(paneId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) {
        if (host_.queueTerminalExit) host_.queueTerminalExit(t);
    };
    pcbs.quit = [this]() { if (host_.quit) host_.quit(); };
    Script::Engine* scriptEngine = host_.scriptEngine;
    pcbs.shouldFilterOutput = [scriptEngine, paneId]() {
        return scriptEngine->hasPaneOutputFilters(paneId);
    };
    pcbs.filterOutput = [scriptEngine, paneId](std::string& data) {
        scriptEngine->filterPaneOutput(paneId, data);
    };
    pcbs.shouldFilterInput = [scriptEngine, paneId]() {
        return scriptEngine->hasPaneInputFilters(paneId);
    };
    pcbs.filterInput = [scriptEngine, paneId](std::string& data) {
        scriptEngine->filterPaneInput(paneId, data);
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

    // Read geometry from the layout tree node (Terminal may not exist yet)
    PaneRect pr;
    for (auto& t : tabs_) {
        pr = t->layout()->nodeRect(paneId);
        if (!pr.isEmpty()) break;
    }
    int cols = (pr.w > 0 && charWidth > 0)  ? static_cast<int>((pr.w - padLeft - padRight) / charWidth)  : 80;
    int rows = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    host_.pending->structuralOps.push_back(PendingMutations::CreatePaneState{paneId, cols, rows});
    host_.pending->dirtyPanes.insert(paneId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();

    // Insert the Terminal into the layout slot created by splitPane()
    for (auto& t : tabs_) {
        if (t->layout()->nodeRect(paneId).isEmpty()) continue;
        t->layout()->insertTerminal(paneId, std::move(terminal));
        break;
    }
    addPtyPoll(masterFD, termPtr);

    if (host_.scriptEngine) host_.scriptEngine->notifyPaneCreated(tabIdx, paneId);
}


void TabManager::resizeAllPanesInTab(Tab* tab)
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

    tab->layout()->computeRects(fbWidth, fbHeight);

    for (auto& panePtr : tab->layout()->panes()) {
        Terminal* pane = panePtr.get();
        pane->resizeToRect(charWidth, lineHeight, padLeft, padTop, padRight, padBottom);

        int cols = std::max(pane->width(),  1);
        int rows = std::max(pane->height(), 1);

        host_.pending->structuralOps.push_back(
            PendingMutations::ResizePaneState{pane->id(), cols, rows});
        host_.pending->dirtyPanes.insert(pane->id());
        host_.pending->releasePaneTextures.push_back(pane->id());

        // Terminal::resize() sets mResizePending if dims changed;
        // flushPendingResize() will send TIOCSWINSZ in the render loop.
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneResized(pane->id(), cols, rows);
    }
    refreshDividers(tab);
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}

// -------------------------------------------------------------------------
// JS-facing primitives
// -------------------------------------------------------------------------

Tab* TabManager::findTabBySubtreeRoot(Uuid subtreeRoot, int* outTabIdx)
{
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        if (tabs_[i]->layout()->subtreeRoot() == subtreeRoot) {
            if (outTabIdx) *outTabIdx = i;
            return tabs_[i].get();
        }
    }
    return nullptr;
}

Tab* TabManager::findTabForNode(Uuid nodeId, int* outTabIdx)
{
    if (!host_.scriptEngine) return nullptr;
    LayoutTree& tree = host_.scriptEngine->layoutTree();
    Uuid cur = nodeId;
    while (!cur.isNil()) {
        if (Tab* t = findTabBySubtreeRoot(cur, outTabIdx)) return t;
        const Node* n = tree.node(cur);
        if (!n) break;
        cur = n->parent;
    }
    return nullptr;
}

int TabManager::findPaneIdByNodeId(Uuid nodeId)
{
    for (auto& tab : tabs_) {
        for (auto& p : tab->layout()->panes()) {
            if (p->nodeId() == nodeId) return p->id();
        }
    }
    return -1;
}

int TabManager::createEmptyTab(Uuid* outNodeId)
{
    const bool headless = host_.headless && host_.headless();
    Window* win = host_.window ? host_.window() : nullptr;
    if (!win && !headless) return -1;

    const int divW = host_.dividerWidth ? host_.dividerWidth() : 0;

    auto layout = std::make_unique<Layout>(host_.scriptEngine
                                           ? &host_.scriptEngine->layoutTree()
                                           : nullptr);
    layout->setDividerPixels(divW);

    const bool barVisible = host_.tabBarVisible ? host_.tabBarVisible() : false;
    const float tbLine = host_.tabBarLineHeight ? host_.tabBarLineHeight() : 0.0f;
    if (barVisible && tbLine > 0.0f) {
        std::string pos = host_.tabBarPosition ? host_.tabBarPosition() : "top";
        layout->setTabBar(static_cast<int>(std::ceil(tbLine)), pos);
    }

    Uuid subRoot = layout->subtreeRoot();
    if (outNodeId) *outNodeId = subRoot;

    int tabIdx = static_cast<int>(tabs_.size());
    auto tab = std::make_unique<Tab>(std::move(layout));
    Tab* tabRaw = tab.get();
    tabs_.push_back(std::move(tab));
    attachLayoutSubtree(tabRaw, /*activate=*/false);

    if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();

    if (host_.scriptEngine) host_.scriptEngine->notifyTabCreated(tabIdx);
    return tabIdx;
}

void TabManager::activateTabByIdx(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    if (Tab* prev = activeTab()) {
        clearDividers(prev);
        releaseTabTextures(prev);
    }
    setActiveTabIdx(idx);
    if (Tab* now = activeTab())
        refreshDividers(now);
    updateWindowTitle();
    if (host_.inputController) host_.inputController->refreshPointerShape();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
}

bool TabManager::createTerminalInContainer(Uuid parentContainerNodeId,
                                           const std::string& cwd,
                                           int* outPaneId,
                                           Uuid* outNodeId)
{
    if (!host_.scriptEngine) return false;
    LayoutTree& tree = host_.scriptEngine->layoutTree();

    int tabIdx = -1;
    Tab* tab = findTabForNode(parentContainerNodeId, &tabIdx);
    if (!tab) return false;
    Layout* layout = tab->layout();

    Uuid newNodeId;
    int paneId = layout->allocatePaneNode(&newNodeId);
    if (!tree.appendChild(parentContainerNodeId, ChildSlot{newNodeId, /*stretch=*/1}))
        return false;

    const uint32_t fbW = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbH = host_.fbHeight ? host_.fbHeight() : 0;
    layout->computeRects(fbW, fbH);

    spawnTerminalForPane(paneId, tabIdx, cwd);

    if (outPaneId) *outPaneId = paneId;
    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool TabManager::splitPaneByNodeId(Uuid existingPaneNodeId, LayoutNode::Dir dir,
                                   float ratio, bool newIsFirst,
                                   int* outPaneId, Uuid* outNodeId)
{
    (void)ratio; // splitByNodeId uses default stretch inheritance
    int tabIdx = -1;
    Tab* tab = findTabForNode(existingPaneNodeId, &tabIdx);
    if (!tab) return false;
    Layout* layout = tab->layout();

    Uuid newNodeId;
    int paneId = layout->allocatePaneNode(&newNodeId);
    if (!layout->splitByNodeId(existingPaneNodeId, dir, newNodeId, newIsFirst))
        return false;

    const uint32_t fbW = host_.fbWidth ? host_.fbWidth() : 0;
    const uint32_t fbH = host_.fbHeight ? host_.fbHeight() : 0;
    layout->computeRects(fbW, fbH);

    std::string cwd;
    if (Terminal* fp = layout->pane(findPaneIdByNodeId(existingPaneNodeId)))
        cwd = paneProcessCWD(fp);

    spawnTerminalForPane(paneId, tabIdx, cwd);
    resizeAllPanesInTab(tab);

    if (outPaneId) *outPaneId = paneId;
    if (outNodeId) *outNodeId = newNodeId;
    return true;
}

bool TabManager::focusPaneById(int paneId)
{
    int tabIdx = -1;
    Tab* tab = findTabForPane(paneId, &tabIdx);
    if (!tab) return false;
    int prev = tab->layout()->focusedPaneId();
    tab->layout()->setFocusedPane(paneId);
    notifyPaneFocusChange(tab, prev, paneId);
    updateTabTitleFromFocusedPane(tabIdx);
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
    return true;
}

bool TabManager::closePaneById(int paneId)
{
    int tabIdx = -1;
    Tab* tab = findTabForPane(paneId, &tabIdx);
    if (!tab) return false;
    Layout* layout = tab->layout();
    if (layout->panes().size() <= 1) return false; // keep last pane
    Terminal* fp = layout->pane(paneId);
    if (!fp) return false;

    removePtyPoll(fp->masterFD());
    host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{paneId});
    for (const auto& popup : fp->popups()) {
        std::string key = std::to_string(paneId) + "/" + popup->popupId();
        host_.pending->releasePopupTextures.push_back(key);
    }
    if (host_.inputController) host_.inputController->erasePaneCursorStyle(paneId);

    if (host_.scriptEngine)
        host_.scriptEngine->notifyPaneDestroyed(paneId, fp->nodeId());

    std::unique_ptr<Terminal> extracted;
    uint64_t stamp = 0;
    {
        std::lock_guard<std::recursive_mutex> plk(*host_.platformMutex);
        extracted = layout->extractPane(paneId);
        if (host_.buildRenderFrameState) host_.buildRenderFrameState();
        stamp = host_.completedFrames ? host_.completedFrames() : 0;
    }
    if (extracted && host_.graveyard)
        host_.graveyard->defer(std::move(extracted), stamp);

    resizeAllPanesInTab(tab);
    notifyPaneFocusChange(tab, -1, layout->focusedPaneId());
    updateTabTitleFromFocusedPane(tabIdx);
    return true;
}
