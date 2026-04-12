#include "PlatformDawn.h"
#include "PlatformUtils.h"
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


// Helper: update the macOS window title from the active tab's focused pane
void PlatformDawn::updateWindowTitle()
{
    if (isHeadless()) return;
    Tab* t = activeTab();
    if (!t) return;
    Pane* fp = t->layout()->focusedPane();
    if (!fp) return;
    const std::string& icon = fp->icon();
    // Prefer the pane's OSC-set title; fall back to the tab title (e.g. foreground process name)
    const std::string& title = fp->title().empty() ? t->title() : fp->title();
    if (title.empty()) return;
    std::string windowTitle = icon.empty() ? title : icon + " " + title;
    if (window_) window_->setTitle(windowTitle);
}

Window::CursorStyle PlatformDawn::pointerShapeNameToCursorStyle(const std::string& name)
{
    using CS = Window::CursorStyle;
    if (name.empty() || name == "default" || name == "left_ptr" ||
        name == "context-menu" || name == "alias" || name == "copy" ||
        name == "dnd-link" || name == "dnd-copy" || name == "dnd-none" ||
        name == "none")                                              return CS::Arrow;
    if (name == "text" || name == "vertical-text" ||
        name == "xterm" || name == "ibeam")                          return CS::IBeam;
    if (name == "pointer" || name == "pointing_hand" || name == "hand" ||
        name == "hand1" || name == "hand2" || name == "openhand" ||
        name == "closedhand" || name == "grab" || name == "grabbing") return CS::Pointer;
    if (name == "crosshair" || name == "tcross" || name == "cross" ||
        name == "cell" || name == "plus")                            return CS::Crosshair;
    if (name == "wait" || name == "clock" || name == "watch" ||
        name == "progress" || name == "half-busy" ||
        name == "left_ptr_watch")                                    return CS::Wait;
    if (name == "help" || name == "question_arrow" ||
        name == "whats_this")                                        return CS::Help;
    if (name == "move" || name == "fleur" || name == "all-scroll" ||
        name == "pointer-move")                                      return CS::Move;
    if (name == "not-allowed" || name == "no-drop" ||
        name == "forbidden" || name == "crossed_circle" ||
        name == "dnd-no-drop")                                       return CS::NotAllowed;
    if (name == "ew-resize" || name == "e-resize" || name == "w-resize" ||
        name == "col-resize" || name == "right_side" ||
        name == "left_side" || name == "sb_h_double_arrow" ||
        name == "split_h")                                           return CS::ResizeH;
    if (name == "ns-resize" || name == "n-resize" || name == "s-resize" ||
        name == "row-resize" || name == "top_side" ||
        name == "bottom_side" || name == "sb_v_double_arrow" ||
        name == "split_v")                                           return CS::ResizeV;
    if (name == "nesw-resize" || name == "ne-resize" || name == "sw-resize" ||
        name == "top_right_corner" || name == "bottom_left_corner" ||
        name == "size_bdiag" || name == "size-bdiag")                return CS::ResizeNESW;
    if (name == "nwse-resize" || name == "nw-resize" || name == "se-resize" ||
        name == "top_left_corner" || name == "bottom_right_corner" ||
        name == "size_fdiag" || name == "size-fdiag")                return CS::ResizeNWSE;
    if (name == "zoom-in" || name == "zoom_in" ||
        name == "zoom-out" || name == "zoom_out")                    return CS::Crosshair;
    return CS::Arrow;
}

// Helper: find which tab contains a given pane
static Tab* findTabForPane(const std::vector<std::unique_ptr<Tab>>& tabs, int paneId, int* outTabIdx = nullptr)
{
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        if (tabs[i]->layout()->pane(paneId)) {
            if (outTabIdx) *outTabIdx = i;
            return tabs[i].get();
        }
    }
    return nullptr;
}

TerminalCallbacks PlatformDawn::buildTerminalCallbacks(int paneId)
{
    TerminalCallbacks cbs;

    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        switch (static_cast<TerminalEmulator::Event>(ev)) {
        case TerminalEmulator::Update:
        case TerminalEmulator::ScrollbackChanged:
            setNeedsRedraw();
            {
                auto it = paneRenderStates_.find(paneId);
                if (it != paneRenderStates_.end()) it->second.dirty = true;
            }
            break;
        case TerminalEmulator::VisibleBell:
            break;
        }
    };

    if (!isHeadless()) {
        cbs.copyToClipboard = [this](const std::string& text) {
            if (window_) window_->setClipboard(text);
        };
        cbs.pasteFromClipboard = [this]() -> std::string {
            return window_ ? window_->getClipboard() : std::string{};
        };
    } else {
        cbs.copyToClipboard = [](const std::string&) {};
        cbs.pasteFromClipboard = []() -> std::string { return {}; };
    }

    cbs.onTitleChanged = [this, paneId](const std::string& title) {
        int tabIdx = -1;
        Tab* t = findTabForPane(tabs_, paneId, &tabIdx);
        if (!t) return;
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) updateWindowTitle();
            tabBarDirty_ = true;
            setNeedsRedraw();
        }
    };

    cbs.onIconChanged = [this, paneId](const std::string& icon) {
        int tabIdx = -1;
        Tab* t = findTabForPane(tabs_, paneId, &tabIdx);
        if (!t) return;
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            if (tabIdx == activeTabIdx_) updateWindowTitle();
            tabBarDirty_ = true;
            setNeedsRedraw();
        }
    };

    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        Tab* t = findTabForPane(tabs_, paneId);
        if (!t) return;
        if (Pane* p = t->layout()->pane(paneId)) {
            p->setProgress(state, pct);
            tabBarDirty_ = true;
            setNeedsRedraw();
        }
    };

    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = isHeadless() ? []() { return true; } : []() { return platformIsDarkMode(); };

    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        Tab* t = findTabForPane(tabs_, paneId);
        if (!t) return;
        if (Pane* p = t->layout()->pane(paneId)) p->setCWD(dir);
    };

    cbs.onDesktopNotification = isHeadless()
        ? [](const std::string&, const std::string&, const std::string&) {}
        : [](const std::string& title, const std::string& body, const std::string&) {
            platformSendNotification(title, body);
        };

    cbs.onOSC = [this, paneId](int oscNum, std::string_view payload) {
        scriptEngine_.notifyOSC(paneId, oscNum, std::string(payload));
    };

    cbs.customTcapLookup = [this](const std::string& name) -> std::optional<std::string> {
        return scriptEngine_.lookupCustomTcap(name);
    };

    cbs.onMouseCursorShape = [this, paneId](const std::string& shape) {
        // Cache the converted style per pane; cursor-pos updates read this
        // without re-parsing the CSS name on every mouse move.
        if (shape.empty()) {
            paneCursorStyle_.erase(paneId);
        } else {
            paneCursorStyle_[paneId] = pointerShapeNameToCursorStyle(shape);
        }
        // Apply immediately if the request is from the focused pane in the
        // active tab; otherwise the next mouse-move into that pane will pick
        // it up. Background panes/tabs don't fight over the cursor.
        if (!window_ || isHeadless()) return;
        Tab* t = activeTab();
        if (!t || t->layout()->focusedPaneId() != paneId) return;
        window_->setCursorStyle(shape.empty()
            ? Window::CursorStyle::IBeam
            : paneCursorStyle_[paneId]);
    };

    cbs.onForegroundProcessChanged = [this, paneId](const std::string& proc) {
        scriptEngine_.notifyForegroundProcessChanged(paneId, proc);
        // Use foreground process as tab title if pane has no OSC title
        int tabIdx = -1;
        Tab* t = findTabForPane(tabs_, paneId, &tabIdx);
        if (!t) return;
        Pane* p = t->layout()->pane(paneId);
        if (p && p->title().empty() && t->layout()->focusedPaneId() == paneId) {
            t->setTitle(proc);
            if (tabIdx == activeTabIdx_) updateWindowTitle();
            tabBarDirty_ = true;
            setNeedsRedraw();
        }
    };

    return cbs;
}


void PlatformDawn::createTab()
{
    if (!device_ || (!window_ && !isHeadless())) return;

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

    auto cbs = buildTerminalCallbacks(paneId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) { terminalExited(t); };
    pcbs.quit = [this]() { quit(); };
    pcbs.shouldFilterOutput = [this, paneId]() {
        return scriptEngine_.hasPaneOutputFilters(paneId);
    };
    pcbs.filterOutput = [this, paneId](std::string& data) {
        scriptEngine_.filterPaneOutput(paneId, data);
    };
    pcbs.shouldFilterInput = [this, paneId]() {
        return scriptEngine_.hasPaneInputFilters(paneId);
    };
    pcbs.filterInput = [this, paneId](std::string& data) {
        scriptEngine_.filterPaneInput(paneId, data);
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
    int cols = (pr.w > 0 && charWidth_ > 0) ? static_cast<int>((pr.w - padLeft_ - padRight_) / charWidth_) : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>((pr.h - padTop_ - padBottom_) / lineHeight_) : 24;

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

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

    updateTabBarVisibility();
    updateWindowTitle();
    refreshPointerShape();
    tabBarDirty_ = true;
    setNeedsRedraw();

    scriptEngine_.notifyTabCreated(tabIdx);
    scriptEngine_.notifyPaneCreated(tabIdx, paneId);

    spdlog::info("Created tab {}", tabIdx + 1);
}


void PlatformDawn::closeTab(int idx)
{
    if (tabs_.empty() || idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    if (tabs_.size() == 1) return; // can't close the last tab

    // Stop PTY polls for all terminals in this tab
    Tab* tab = tabs_[idx].get();
    for (auto& panePtr : tab->layout()->panes()) {
        if (auto* t = panePtr->terminal()) {
            removePtyPoll(t->masterFD());
        }
        // Release pane and popup render states
        auto it = paneRenderStates_.find(panePtr->id());
        if (it != paneRenderStates_.end()) {
            if (it->second.heldTexture)
                pendingTabBarRelease_.push_back(it->second.heldTexture);
            for (auto* t2 : it->second.pendingRelease)
                pendingTabBarRelease_.push_back(t2);
            paneRenderStates_.erase(it);
        }
        paneCursorStyle_.erase(panePtr->id());
        releasePopupStates(panePtr.get());
        scriptEngine_.notifyPaneDestroyed(panePtr->id());
    }

    if (tab->hasOverlay())
        scriptEngine_.notifyOverlayDestroyed(idx);
    scriptEngine_.notifyTabDestroyed(idx);

    tabs_.erase(tabs_.begin() + idx);

    // Adjust active tab index
    if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
        activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;

    updateTabBarVisibility();
    refreshPointerShape();
    tabBarDirty_ = true;
    setNeedsRedraw();
    spdlog::info("Closed tab {}", idx + 1);
}


void PlatformDawn::addPtyPoll(int fd, Terminal* term)
{
    term->setLoop(eventLoop_.get());
    eventLoop_->watchFd(fd, EventLoop::FdEvents::Readable,
        [term](EventLoop::FdEvents ev) {
            if (ev & EventLoop::FdEvents::Readable) term->readFromFD();
            if (ev & EventLoop::FdEvents::Writable) term->flushWriteQueue();
        });
    ptyPolls_[fd] = term;
}


void PlatformDawn::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) return;
    if (eventLoop_) eventLoop_->removeFd(fd);
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
        if (p) {
            // Clear popup focus when pane loses focus
            p->clearFocusedPopup();
            if (p->terminal()) p->terminal()->focusEvent(false);
        }
    }
    if (newId >= 0) {
        Pane* p = tab->layout()->pane(newId);
        if (p && p->terminal()) p->terminal()->focusEvent(true);
    }
    refreshPointerShape();
}

void PlatformDawn::refreshPointerShape()
{
    if (!window_ || isHeadless()) return;
    Tab* tab = activeTab();
    if (!tab) return;
    // Prefer the pane the mouse is physically over (so split/focus changes
    // don't show a cursor that doesn't match the hovered pane). Falls back to
    // the focused pane when the mouse position isn't usefully hovering one
    // (e.g. before any motion event has fired).
    int paneId = -1;
    if (!tab->hasOverlay()) {
        double sx = lastCursorX_ * contentScaleX_;
        double sy = lastCursorY_ * contentScaleY_;
        paneId = tab->layout()->paneAtPixel(static_cast<int>(sx),
                                            static_cast<int>(sy));
        if (paneId < 0 && tab->layout()->focusedPane())
            paneId = tab->layout()->focusedPane()->id();
    }
    auto it = paneCursorStyle_.find(paneId);
    window_->setCursorStyle(it != paneCursorStyle_.end()
        ? it->second
        : Window::CursorStyle::IBeam);
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
        updateWindowTitle();
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
            paneCursorStyle_.erase(paneId);
            releasePopupStates(panePtr.get());

            scriptEngine_.notifyPaneDestroyed(paneId);

            if (tab->layout()->panes().size() <= 1) {
                // Last pane in the tab — close the tab
                if (tab->hasOverlay())
                    scriptEngine_.notifyOverlayDestroyed(tabIdx);
                scriptEngine_.notifyTabDestroyed(tabIdx);
                tabs_.erase(tabs_.begin() + tabIdx);
                if (tabs_.empty()) {
                    quit();
                    return;
                }
                if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
                    activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
                updateTabBarVisibility();
                updateWindowTitle();
                refreshPointerShape();
                tabBarDirty_ = true;
            } else {
                tab->layout()->removePane(paneId);
                resizeAllPanesInTab(tab);
                notifyPaneFocusChange(tab, -1, tab->layout()->focusedPaneId());
                updateTabTitleFromFocusedPane(tabIdx);
            }

            setNeedsRedraw();
            return;
        }
    }
    // Fallback: terminal not found in any pane
    quit();
}


void PlatformDawn::spawnTerminalForPane(Pane* pane, int tabIdx, const std::string& cwd)
{
    int paneId = pane->id();

    auto cbs = buildTerminalCallbacks(paneId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) { terminalExited(t); };
    pcbs.quit = [this]() { quit(); };
    pcbs.shouldFilterOutput = [this, paneId]() {
        return scriptEngine_.hasPaneOutputFilters(paneId);
    };
    pcbs.filterOutput = [this, paneId](std::string& data) {
        scriptEngine_.filterPaneOutput(paneId, data);
    };
    pcbs.shouldFilterInput = [this, paneId]() {
        return scriptEngine_.hasPaneInputFilters(paneId);
    };
    pcbs.filterInput = [this, paneId](std::string& data) {
        scriptEngine_.filterPaneInput(paneId, data);
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
    int cols = (pr.w > 0 && charWidth_ > 0)  ? static_cast<int>((pr.w - padLeft_ - padRight_) / charWidth_)  : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>((pr.h - padTop_ - padBottom_) / lineHeight_) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);

    scriptEngine_.notifyPaneCreated(tabIdx, paneId);
}


void PlatformDawn::resizeAllPanesInTab(Tab* tab)
{
    if (!tab) return;
    clearDividers(tab);
    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

        Terminal* term = pane->terminal();
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

        // Terminal::resize() sets mResizePending if dims changed;
        // flushPendingResize() will send TIOCSWINSZ in the render loop.
        scriptEngine_.notifyPaneResized(pane->id(), cols, rows);
    }
    refreshDividers(tab);
    setNeedsRedraw();
}

