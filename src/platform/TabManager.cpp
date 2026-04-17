#include "TabManager.h"

#include "InputController.h"
#include "Layout.h"
#include "Pane.h"
#include "PlatformUtils.h"
#include "ScriptEngine.h"
#include "Terminal.h"

#include <eventloop/EventLoop.h>
#include <eventloop/Window.h>

#include <spdlog/spdlog.h>

#include <cmath>

static void collectFirstPaneDividers(const LayoutNode* node, int divPx,
                                      std::vector<std::pair<int, PaneRect>>& out)
{
    if (!node || node->isLeaf || divPx <= 0) return;
    const PaneRect& r = node->rect;

    // The "first" (left/top) child owns this divider.
    int firstPaneId = -1;
    const LayoutNode* first = node->first.get();
    while (first && !first->isLeaf) first = first->first.get(); // leftmost leaf
    if (first) firstPaneId = first->paneId;

    if (firstPaneId >= 0) {
        PaneRect divRect;
        if (node->dir == LayoutNode::Dir::Horizontal) {
            int splitX = node->first ? (node->first->rect.x + node->first->rect.w) : 0;
            divRect = {splitX, r.y, divPx, r.h};
        } else {
            int splitY = node->first ? (node->first->rect.y + node->first->rect.h) : 0;
            divRect = {r.x, splitY, r.w, divPx};
        }
        out.push_back({firstPaneId, divRect});
    }

    collectFirstPaneDividers(node->first.get(),  divPx, out);
    collectFirstPaneDividers(node->second.get(), divPx, out);
}


Terminal* TabManager::activeTerm()
{
    Tab* tab = activeTab();
    if (!tab) return nullptr;
    if (tab->hasOverlay()) return tab->topOverlay();
    Pane* pane = tab->layout()->focusedPane();
    return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
}


void TabManager::notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn)
{
    for (auto& tab : tabs_) {
        for (auto& panePtr : tab->layout()->panes()) {
            if (auto* term = panePtr->terminal()) fn(term);
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


void TabManager::addInitialTab(std::unique_ptr<Tab> tab)
{
    activeTabIdx_ = static_cast<int>(tabs_.size());
    tabs_.push_back(std::move(tab));
}


void TabManager::updateWindowTitle()
{
    if (host_.headless && host_.headless()) return;
    Tab* t = activeTab();
    if (!t) return;
    Pane* fp = t->layout()->focusedPane();
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

    // Collect (paneId, dividerRect) for each split node
    std::vector<std::pair<int, PaneRect>> dividers;
    collectFirstPaneDividers(layout->root(), divPx, dividers);

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
        Pane* p = tab->layout()->pane(prevId);
        if (p) {
            if (!p->focusedPopupId().empty() && host_.scriptEngine)
                host_.scriptEngine->notifyFocusedPopupChanged(prevId, "");
            p->clearFocusedPopup();
            if (p->terminal()) p->terminal()->focusEvent(false);
        }
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(prevId, false);
    }
    if (newId >= 0) {
        Pane* p = tab->layout()->pane(newId);
        if (p && p->terminal()) p->terminal()->focusEvent(true);
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneFocusChanged(newId, true);
    }
    if (host_.inputController) host_.inputController->refreshPointerShape();
}


void TabManager::updateTabTitleFromFocusedPane(int tabIdx)
{
    if (tabIdx < 0 || tabIdx >= static_cast<int>(tabs_.size())) return;
    Tab* tab = tabs_[tabIdx].get();
    Pane* fp = tab->layout()->focusedPane();
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

    auto layout = std::make_unique<Layout>();
    layout->setDividerPixels(divW);
    Pane* pane = layout->createPane();
    layout->setFocusedPane(pane->id());

    // Apply tab bar height to the new layout
    const bool barVisible = host_.tabBarVisible ? host_.tabBarVisible() : false;
    const float tbLine = host_.tabBarLineHeight ? host_.tabBarLineHeight() : 0.0f;
    if (barVisible && tbLine > 0.0f) {
        std::string pos = host_.tabBarPosition ? host_.tabBarPosition() : "top";
        layout->setTabBar(static_cast<int>(std::ceil(tbLine)), pos);
    }
    layout->computeRects(fbWidth, fbHeight);

    int paneId = pane->id();
    int tabIdx = static_cast<int>(tabs_.size());

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
    // Inherit CWD from the focused pane of the active tab
    auto opts = terminalOptions_;
    if (Tab* at = activeTab()) {
        if (Pane* fp = at->layout()->focusedPane()) {
            std::string cwd = paneProcessCWD(fp);
            if (!cwd.empty()) opts.cwd = cwd;
        }
    }

    terminal->applyColorScheme(opts.colors);
    terminal->applyCursorConfig(opts.cursor);
    if (!terminal->init(opts)) {
        spdlog::error("createTab: failed to init terminal");
        return;
    }

    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth > 0) ? static_cast<int>((pr.w - padLeft - padRight) / charWidth) : 80;
    int rows = (pr.h > 0 && lineHeight > 0) ? static_cast<int>((pr.h - padTop - padBottom) / lineHeight) : 24;

    host_.pending->structuralOps.push_back(PendingMutations::CreatePaneState{paneId, cols, rows});
    host_.pending->dirtyPanes.insert(paneId);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);

    activeTabIdx_ = tabIdx;
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(pane->title());
    tabs_.push_back(std::move(tab));

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

    // Stop PTY polls for all terminals in this tab
    Tab* tab = tabs_[idx].get();
    for (auto& panePtr : tab->layout()->panes()) {
        if (auto* t = panePtr->terminal()) {
            removePtyPoll(t->masterFD());
        }
        // Queue render state cleanup
        host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{panePtr->id()});
        for (const auto& popup : panePtr->popups()) {
            std::string key = popupStateKey(panePtr->id(), popup.id);
            host_.pending->releasePopupTextures.push_back(key);
        }
        if (host_.inputController) host_.inputController->erasePaneCursorStyle(panePtr->id());
        if (host_.scriptEngine) host_.scriptEngine->notifyPaneDestroyed(panePtr->id());
    }

    if (tab->hasOverlay()) {
        if (host_.scriptEngine) host_.scriptEngine->notifyOverlayDestroyed(idx);
        host_.pending->structuralOps.push_back(PendingMutations::DestroyOverlayState{});
    }
    if (host_.scriptEngine) host_.scriptEngine->notifyTabDestroyed(idx);

    tabs_.erase(tabs_.begin() + idx);

    // Adjust active tab index
    if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
        activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;

    if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
    if (host_.inputController) host_.inputController->refreshPointerShape();
    if (host_.markTabBarDirty) host_.markTabBarDirty();
    if (host_.setNeedsRedraw) host_.setNeedsRedraw();
    spdlog::info("Closed tab {}", idx + 1);
}


void TabManager::terminalExited(Terminal* terminal)
{
    // Find which tab and pane owns this terminal
    for (int tabIdx = 0; tabIdx < static_cast<int>(tabs_.size()); ++tabIdx) {
        Tab* tab = tabs_[tabIdx].get();
        for (auto& panePtr : tab->layout()->panes()) {
            if (panePtr->terminal() != terminal) continue;

            int paneId = panePtr->id();
            removePtyPoll(terminal->masterFD());

            host_.pending->structuralOps.push_back(PendingMutations::DestroyPaneState{paneId});
            for (const auto& popup : panePtr->popups()) {
                std::string key = popupStateKey(paneId, popup.id);
                host_.pending->releasePopupTextures.push_back(key);
            }
            if (host_.inputController) host_.inputController->erasePaneCursorStyle(paneId);

            if (host_.scriptEngine) host_.scriptEngine->notifyPaneDestroyed(paneId);

            if (tab->layout()->panes().size() <= 1) {
                // Last pane in the tab — close the tab
                if (tab->hasOverlay() && host_.scriptEngine)
                    host_.scriptEngine->notifyOverlayDestroyed(tabIdx);
                if (host_.scriptEngine) host_.scriptEngine->notifyTabDestroyed(tabIdx);
                tabs_.erase(tabs_.begin() + tabIdx);
                if (tabs_.empty()) {
                    if (host_.quit) host_.quit();
                    return;
                }
                if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
                    activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
                if (host_.updateTabBarVisibility) host_.updateTabBarVisibility();
                updateWindowTitle();
                if (host_.inputController) host_.inputController->refreshPointerShape();
                if (host_.markTabBarDirty) host_.markTabBarDirty();
            } else {
                tab->layout()->removePane(paneId);
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


void TabManager::spawnTerminalForPane(Pane* pane, int tabIdx, const std::string& cwd)
{
    int paneId = pane->id();

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

    const PaneRect& pr = pane->rect();
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
    pane->setTerminal(std::move(terminal));
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
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth, lineHeight, padLeft, padTop, padRight, padBottom);

        Terminal* term = pane->terminal();
        if (!term) continue;

        int cols = std::max(term->width(),  1);
        int rows = std::max(term->height(), 1);

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
