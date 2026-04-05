#include "PlatformDawn.h"
#include "Log.h"
#include "Utils.h"
#include <sys/ioctl.h>

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


void PlatformDawn::createTab()
{
    if (!device_ || !glfwWindow_) return;

    auto layout = std::make_unique<Layout>();
    layout->setDividerPixels(dividerWidth_);
    Pane* pane = layout->createPane();
    layout->setFocusedPane(pane->id());

    // Apply tab bar height to the new layout
    if (tabBarVisible() && tabBarLineHeight_ > 0.0f) {
        layout->setTabBar(static_cast<int>(std::ceil(tabBarLineHeight_)), tabBarConfig_.position);
    }
    layout->computeRects(fbWidth_, fbHeight_);

    int paneId = pane->id();
    int tabIdx = static_cast<int>(tabs_.size());

    TerminalCallbacks cbs;
    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        if (ev == TerminalEmulator::Update || ev == TerminalEmulator::ScrollbackChanged) {
            needsRedraw_ = true;
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        }
    };
    cbs.copyToClipboard = [this](const std::string& text) {
        glfwSetClipboardString(glfwWindow_, text.c_str());
    };
    cbs.pasteFromClipboard = [this]() -> std::string {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        return clip ? std::string(clip) : std::string();
    };
    cbs.onTitleChanged = [this, paneId, tabIdx](const std::string& title) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) glfwSetWindowTitle(glfwWindow_, title.c_str());
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onIconChanged = [this, paneId, tabIdx](const std::string& icon) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                needsRedraw_ = true;
                break;
            }
        }
    };
    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = []() { return platformIsDarkMode(); };
    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setCWD(dir);
                break;
            }
        }
    };
    cbs.onDesktopNotification = [](const std::string& title, const std::string& body, const std::string&) {
        platformSendNotification(title, body);
    };

    PlatformCallbacks pcbs;
        pcbs.onTerminalExited = [this](Terminal* t) { terminalExited(t); };
        pcbs.quit = [this]() { quit(); };
        auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    terminal->applyColorScheme(terminalOptions_.colors);
    if (!terminal->init(terminalOptions_)) {
        spdlog::error("createTab: failed to init terminal");
        return;
    }

    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth_ > 0) ? static_cast<int>((pr.w - padLeft_ - padRight_) / charWidth_) : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>((pr.h - padTop_ - padBottom_) / lineHeight_) : 24;

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

    terminal->resize(cols, rows);
    {
        struct winsize ws = {};
        ws.ws_col    = static_cast<unsigned short>(cols);
        ws.ws_row    = static_cast<unsigned short>(rows);
        ws.ws_xpixel = static_cast<unsigned short>(fbWidth_);
        ws.ws_ypixel = static_cast<unsigned short>(fbHeight_);
        ioctl(terminal->masterFD(), TIOCSWINSZ, &ws);
    }

    // Set initial title from shell name
    {
        std::string shellName = terminalOptions_.shell;
        auto slash = shellName.rfind('/');
        if (slash != std::string::npos) shellName = shellName.substr(slash + 1);
        pane->setTitle(shellName);
    }

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);

    activeTabIdx_ = tabIdx;
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(pane->title());
    tabs_.push_back(std::move(tab));

    updateTabBarVisibility();
    tabBarDirty_ = true;
    needsRedraw_ = true;

    spdlog::info("Created tab {}", tabIdx + 1);
}


void PlatformDawn::closeTab(int idx)
{
    if (tabs_.empty() || idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    if (tabs_.size() == 1) return; // can't close the last tab

    // Stop PTY polls for all terminals in this tab
    Tab* tab = tabs_[idx].get();
    for (auto& panePtr : tab->layout()->panes()) {
        TerminalEmulator* term = panePtr->activeTerm();
        if (auto* t = dynamic_cast<Terminal*>(term)) {
            removePtyPoll(t->masterFD());
        }
        // Release pane render state
        auto it = paneRenderStates_.find(panePtr->id());
        if (it != paneRenderStates_.end()) {
            if (it->second.heldTexture)
                pendingTabBarRelease_.push_back(it->second.heldTexture);
            for (auto* t2 : it->second.pendingRelease)
                pendingTabBarRelease_.push_back(t2);
            paneRenderStates_.erase(it);
        }
    }

    tabs_.erase(tabs_.begin() + idx);

    // Adjust active tab index
    if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
        activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;

    updateTabBarVisibility();
    tabBarDirty_ = true;
    needsRedraw_ = true;
    spdlog::info("Closed tab {}", idx + 1);
}


void PlatformDawn::addPtyPoll(int fd, Terminal* term)
{
    auto* poll = new uv_poll_t{};
    uv_poll_init(loop_, poll, fd);
    poll->data = term;
    uv_poll_start(poll, UV_READABLE, [](uv_poll_t* handle, int status, int events) {
        if (status < 0) return;
        if (events & UV_READABLE)
            static_cast<Terminal*>(handle->data)->readFromFD();
    });
    ptyPolls_[fd] = poll;
}


void PlatformDawn::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) return;
    uv_poll_stop(it->second);
    uv_close(reinterpret_cast<uv_handle_t*>(it->second), [](uv_handle_t* h) {
        delete reinterpret_cast<uv_poll_t*>(h);
    });
    ptyPolls_.erase(it);
}


void PlatformDawn::refreshDividers(Tab* tab)
{
    if (!tab || dividerWidth_ <= 0) return;
    Layout* layout = tab->layout();

    // Clear all divider VBs for this tab's panes
    for (auto& panePtr : layout->panes())
        paneRenderStates_[panePtr->id()].dividerVB = {};

    if (layout->panes().size() <= 1 || layout->isZoomed()) return;

    // Collect (paneId, dividerRect) for each split node
    std::vector<std::pair<int, PaneRect>> dividers;
    collectFirstPaneDividers(layout->root(), dividerWidth_, dividers);

    renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_);

    for (auto& [paneId, dr] : dividers) {
        auto it = paneRenderStates_.find(paneId);
        if (it == paneRenderStates_.end()) continue;
        renderer_.updateDividerBuffer(queue_, it->second.dividerVB,
            static_cast<float>(dr.x), static_cast<float>(dr.y),
            static_cast<float>(dr.w), static_cast<float>(dr.h),
            dividerR_, dividerG_, dividerB_, dividerA_);
    }
}


void PlatformDawn::clearDividers(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes())
        paneRenderStates_[panePtr->id()].dividerVB = {};
}


void PlatformDawn::notifyPaneFocusChange(Tab* tab, int prevId, int newId)
{
    if (!tab) return;
    if (prevId >= 0) {
        Pane* p = tab->layout()->pane(prevId);
        if (p && p->activeTerm()) p->activeTerm()->focusEvent(false);
    }
    if (newId >= 0) {
        Pane* p = tab->layout()->pane(newId);
        if (p && p->activeTerm()) p->activeTerm()->focusEvent(true);
    }
}


void PlatformDawn::updateTabTitleFromFocusedPane(int tabIdx)
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
        glfwSetWindowTitle(glfwWindow_, title.c_str());
    tabBarDirty_ = true;
    needsRedraw_  = true;
}


void PlatformDawn::releaseTabTextures(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes()) {
        auto it = paneRenderStates_.find(panePtr->id());
        if (it == paneRenderStates_.end()) continue;
        PaneRenderState& rs = it->second;
        if (rs.heldTexture) {
            pendingTabBarRelease_.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
            rs.dirty = true;
        }
    }
}


void PlatformDawn::terminalExited(Terminal* terminal)
{
    // Find which tab and pane owns this terminal
    for (int tabIdx = 0; tabIdx < static_cast<int>(tabs_.size()); ++tabIdx) {
        Tab* tab = tabs_[tabIdx].get();
        for (auto& panePtr : tab->layout()->panes()) {
            if (panePtr->terminal() != terminal) continue;

            int paneId = panePtr->id();
            removePtyPoll(terminal->masterFD());

            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) {
                if (it->second.heldTexture)
                    pendingTabBarRelease_.push_back(it->second.heldTexture);
                for (auto* tx : it->second.pendingRelease)
                    pendingTabBarRelease_.push_back(tx);
                paneRenderStates_.erase(it);
            }

            if (tab->layout()->panes().size() <= 1) {
                // Last pane in the tab — close the tab
                tabs_.erase(tabs_.begin() + tabIdx);
                if (tabs_.empty()) {
                    quit();
                    return;
                }
                if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
                    activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
                tabBarDirty_ = true;
            } else {
                tab->layout()->removePane(paneId);
                resizeAllPanesInTab(tab);
                notifyPaneFocusChange(tab, -1, tab->layout()->focusedPaneId());
                updateTabTitleFromFocusedPane(tabIdx);
            }

            needsRedraw_ = true;
            return;
        }
    }
    // Fallback: terminal not found in any pane
    quit();
}


void PlatformDawn::spawnTerminalForPane(Pane* pane, int tabIdx)
{
    int paneId = pane->id();

    TerminalCallbacks cbs;
    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        if (ev == TerminalEmulator::Update || ev == TerminalEmulator::ScrollbackChanged) {
            needsRedraw_ = true;
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        }
    };
    cbs.copyToClipboard = [this](const std::string& text) {
        glfwSetClipboardString(glfwWindow_, text.c_str());
    };
    cbs.pasteFromClipboard = [this]() -> std::string {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        return clip ? std::string(clip) : std::string();
    };
    cbs.onTitleChanged = [this, paneId, tabIdx](const std::string& title) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) glfwSetWindowTitle(glfwWindow_, title.c_str());
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onIconChanged = [this, paneId, tabIdx](const std::string& icon) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                needsRedraw_ = true;
                break;
            }
        }
    };
    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = []() { return platformIsDarkMode(); };
    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setCWD(dir);
                break;
            }
        }
    };
    cbs.onDesktopNotification = [](const std::string& title, const std::string& body, const std::string&) {
        platformSendNotification(title, body);
    };

    PlatformCallbacks pcbs;
        pcbs.onTerminalExited = [this](Terminal* t) { terminalExited(t); };
        pcbs.quit = [this]() { quit(); };
        auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    terminal->applyColorScheme(terminalOptions_.colors);
    if (!terminal->init(terminalOptions_)) {
        spdlog::error("spawnTerminalForPane: failed to init terminal");
        return;
    }

    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth_ > 0)  ? static_cast<int>(pr.w / charWidth_)  : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>(pr.h / lineHeight_) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

    terminal->resize(cols, rows);
    {
        struct winsize ws = {};
        ws.ws_col    = static_cast<unsigned short>(cols);
        ws.ws_row    = static_cast<unsigned short>(rows);
        ws.ws_xpixel = static_cast<unsigned short>(pr.w);
        ws.ws_ypixel = static_cast<unsigned short>(pr.h);
        ioctl(terminal->masterFD(), TIOCSWINSZ, &ws);
    }

    // Set initial title from shell name
    {
        std::string shellName = terminalOptions_.shell;
        auto slash = shellName.rfind('/');
        if (slash != std::string::npos) shellName = shellName.substr(slash + 1);
        pane->setTitle(shellName);
    }

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);
}


void PlatformDawn::resizeAllPanesInTab(Tab* tab)
{
    if (!tab) return;
    clearDividers(tab);
    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        int cols = std::max(term->width(),  1);
        int rows = std::max(term->height(), 1);

        auto& rs = paneRenderStates_[pane->id()];
        rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
        rs.dirty = true;
        if (rs.heldTexture) {
            rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
        }

        if (auto* t = dynamic_cast<Terminal*>(term); t && t->masterFD() >= 0) {
            struct winsize ws = {};
            ws.ws_col    = static_cast<unsigned short>(cols);
            ws.ws_row    = static_cast<unsigned short>(rows);
            ws.ws_xpixel = static_cast<unsigned short>(pane->rect().w);
            ws.ws_ypixel = static_cast<unsigned short>(pane->rect().h);
            ioctl(t->masterFD(), TIOCSWINSZ, &ws);
        }
    }
    refreshDividers(tab);
    needsRedraw_ = true;
}

