#include "PlatformDawn.h"
#include "PlatformUtils.h"
#include "Config.h"
#include <unistd.h>



void PlatformDawn::dispatchAction(const Action::Any& action)
{
    if (actionRouter_) actionRouter_->dispatch(action);
    else executeAction(action);
}

void PlatformDawn::executeAction(const Action::Any& action)
{
    std::visit(overloaded {
        [&](const Action::NewTab&)  { createTab(); },
        [&](const Action::CloseTab& a) { closeTab(a.index >= 0 ? a.index : tabManager_->activeTabIdx()); },
        [&](const Action::ActivateTabRelative& a) {
            int idx = tabManager_->activeTabIdx() + a.delta;
            if (idx >= 0 && idx < static_cast<int>(tabManager_->size())) {
                if (Tab* prev = activeTab()) {
                    tabManager_->clearDividers(prev);
                    tabManager_->releaseTabTextures(prev);
                }
                tabManager_->setActiveTabIdx(idx);
                if (Tab* now = activeTab())
                    tabManager_->refreshDividers(now);
                tabManager_->updateWindowTitle();
                if (inputController_) inputController_->refreshPointerShape();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        },
        [&](const Action::ActivateTab& a) {
            if (a.index >= 0 && a.index < static_cast<int>(tabManager_->size())) {
                if (Tab* prev = activeTab()) {
                    tabManager_->clearDividers(prev);
                    tabManager_->releaseTabTextures(prev);
                }
                tabManager_->setActiveTabIdx(a.index);
                if (Tab* now = activeTab())
                    tabManager_->refreshDividers(now);
                tabManager_->updateWindowTitle();
                if (inputController_) inputController_->refreshPointerShape();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        },
        [&](const Action::SplitPane& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;

            LayoutNode::Dir dir;
            bool newIsFirst = false;
            switch (a.dir) {
            case Action::Direction::Right: dir = LayoutNode::Dir::Horizontal; break;
            case Action::Direction::Left:  dir = LayoutNode::Dir::Horizontal; newIsFirst = true; break;
            case Action::Direction::Down:  dir = LayoutNode::Dir::Vertical;   break;
            case Action::Direction::Up:    dir = LayoutNode::Dir::Vertical;   newIsFirst = true; break;
            default: return;
            }

            int newId = layout->splitPane(fp->id(), dir, 0.5f, newIsFirst);
            if (newId < 0) return;

            Pane* newPane = layout->pane(newId);
            if (!newPane) return;

            layout->computeRects(fbWidth_, fbHeight_);
            int tabIdx = tabManager_->activeTabIdx();
            int prevId = layout->focusedPaneId();
            tabManager_->spawnTerminalForPane(newPane, tabIdx, paneProcessCWD(fp));
            tabManager_->resizeAllPanesInTab(tab);
            layout->setFocusedPane(newId);
            tabManager_->notifyPaneFocusChange(tab, prevId, newId);
            tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
        },
        [&](const Action::ClosePane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            if (layout->panes().size() <= 1) return; // keep last pane
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            int paneId = fp->id();

            // Stop PTY poll and queue render state cleanup
            if (auto* t = fp->terminal()) {
                tabManager_->removePtyPoll(t->masterFD());
            }
            renderThread_->pending().structuralOps.push_back(PendingMutations::DestroyPaneState{paneId});
            // Queue popup render state cleanup for this pane
            for (const auto& popup : fp->popups()) {
                std::string key = popupStateKey(paneId, popup.id);
                renderThread_->pending().releasePopupTextures.push_back(key);
            }
            if (inputController_) inputController_->erasePaneCursorStyle(paneId);

            scriptEngine_.notifyPaneDestroyed(paneId);
            layout->removePane(paneId);
            tabManager_->resizeAllPanesInTab(tab);
            tabManager_->notifyPaneFocusChange(tab, -1, layout->focusedPaneId());
            tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
        },
        [&](const Action::ZoomPane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            layout->zoomPane(fp->id());
            tabManager_->resizeAllPanesInTab(tab);
        },
        [&](const Action::FocusPane& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();

            if (a.dir == Action::Direction::Next || a.dir == Action::Direction::Prev) {
                const auto& panes = layout->panes();
                if (panes.size() <= 1) return;
                int cur = -1;
                for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                    if (panes[i]->id() == layout->focusedPaneId()) { cur = i; break; }
                }
                if (cur < 0) return;
                int n = static_cast<int>(panes.size());
                int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                int next = ((cur + delta) % n + n) % n;
                int prev = layout->focusedPaneId();
                layout->setFocusedPane(panes[next]->id());
                tabManager_->notifyPaneFocusChange(tab, prev, panes[next]->id());
                tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
                setNeedsRedraw();
                return;
            }

            Pane* fp = layout->focusedPane();
            if (!fp) return;
            const PaneRect& r = fp->rect();
            int px = 0, py = 0;
            switch (a.dir) {
            case Action::Direction::Left:  px = r.x - 1;       py = r.y + r.h / 2; break;
            case Action::Direction::Right: px = r.x + r.w + 1; py = r.y + r.h / 2; break;
            case Action::Direction::Up:    px = r.x + r.w / 2; py = r.y - 1;       break;
            case Action::Direction::Down:  px = r.x + r.w / 2; py = r.y + r.h + 1; break;
            default: return;
            }
            int targetId = layout->paneAtPixel(px, py);
            if (targetId >= 0 && targetId != fp->id()) {
                int prev = layout->focusedPaneId();
                layout->setFocusedPane(targetId);
                tabManager_->notifyPaneFocusChange(tab, prev, targetId);
                tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
                setNeedsRedraw();
            }
        },
        [&](const Action::AdjustPaneSize& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;

            LayoutNode::Dir splitDir;
            int deltaPixels;
            switch (a.dir) {
            case Action::Direction::Left:
                splitDir   = LayoutNode::Dir::Horizontal;
                deltaPixels = -static_cast<int>(a.amount * charWidth_);
                break;
            case Action::Direction::Right:
                splitDir   = LayoutNode::Dir::Horizontal;
                deltaPixels = static_cast<int>(a.amount * charWidth_);
                break;
            case Action::Direction::Up:
                splitDir   = LayoutNode::Dir::Vertical;
                deltaPixels = -static_cast<int>(a.amount * lineHeight_);
                break;
            case Action::Direction::Down:
                splitDir   = LayoutNode::Dir::Vertical;
                deltaPixels = static_cast<int>(a.amount * lineHeight_);
                break;
            default: return;
            }
            if (layout->growPane(fp->id(), splitDir, deltaPixels))
                tabManager_->resizeAllPanesInTab(tab);
        },
        [&](const Action::Copy&) {
            Terminal* term = activeTerm();
            if (term && term->hasSelection()) {
                std::string text = term->selectedText();
                if (!text.empty())
                    if (window_) window_->setClipboard(text);
            }
        },
        [&](const Action::Paste&) {
            Terminal* term = activeTerm();
            std::string clip = window_ ? window_->getClipboard() : std::string{};
            if (term && !clip.empty())
                term->pasteText(clip);
        },
        [&](const Action::ScrollUp& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(a.lines);
        },
        [&](const Action::ScrollDown& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(-a.lines);
        },
        [&](const Action::ScrollToTop&) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(std::numeric_limits<int>::max());
        },
        [&](const Action::ScrollToBottom&) {
            Terminal* term = activeTerm();
            if (term) term->resetViewport();
        },
        [&](const Action::PushOverlay&) { /* TODO */ },
        [&](const Action::PopOverlay&) {
            Tab* tab = activeTab();
            if (tab && tab->hasOverlay()) {
                scriptEngine_.notifyOverlayDestroyed(tabManager_->activeTabIdx());
                std::lock_guard<std::mutex> plk(renderThread_->mutex());
                tab->popOverlay();
                renderThread_->renderState().hasOverlay = false;
                renderThread_->renderState().overlay = nullptr;
                if (renderEngine_) renderEngine_->clearOverlayFromFrameState();
                renderThread_->pending().structuralOps.push_back(PendingMutations::DestroyOverlayState{});
                if (inputController_) inputController_->refreshPointerShape();
                setNeedsRedraw();
            }
        },
        [&](const Action::IncreaseFontSize&) { adjustFontSize(1.0f);  },
        [&](const Action::DecreaseFontSize&) { adjustFontSize(-1.0f); },
        [&](const Action::ResetFontSize&)    { adjustFontSize(0.0f);  },
        [&](const Action::ScrollToPrompt& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollToPrompt(a.direction);
        },
        [&](const Action::SelectCommandOutput&) {
            Terminal* term = activeTerm();
            if (term) term->selectCommandOutput();
        },
        [&](const Action::CopyLastCommand&) {
            Terminal* term = activeTerm();
            if (!term) return;
            const auto* cmd = term->lastCommand();
            if (!cmd || !cmd->complete) return;
            // Extract from prompt start through output end (prompt + input + output).
            std::string text = term->document().getTextFromRows(
                cmd->promptStartRowId, cmd->outputEndRowId,
                cmd->promptStartCol,   cmd->outputEndCol);
            if (!text.empty() && window_) window_->setClipboard(text);
        },
        [&](const Action::CopyDocument&) {
            Terminal* term = activeTerm();
            if (!term) return;
            std::string text = term->serializeScrollback();
            if (!text.empty() && window_) window_->setClipboard(text);
        },
        [&](const Action::FocusPopup&) {
            Tab* tab = activeTab();
            if (!tab || tab->hasOverlay()) return;
            Pane* fp = tab->layout()->focusedPane();
            if (!fp || fp->popups().empty()) return;

            const auto& popups = fp->popups();
            const std::string& curFocus = fp->focusedPopupId();

            Terminal* mainTerm = fp->terminal();
            if (curFocus.empty()) {
                // No popup focused — focus first popup
                fp->setFocusedPopup(popups.front().id);
                if (mainTerm) mainTerm->focusEvent(false);
                scriptEngine_.notifyFocusedPopupChanged(fp->id(), popups.front().id);
            } else {
                // Find current popup index, cycle to next or back to main
                int cur = -1;
                for (int i = 0; i < static_cast<int>(popups.size()); ++i) {
                    if (popups[i].id == curFocus) { cur = i; break; }
                }
                if (cur < 0 || cur + 1 >= static_cast<int>(popups.size())) {
                    // Last popup or not found — back to main terminal
                    fp->clearFocusedPopup();
                    if (mainTerm) mainTerm->focusEvent(true);
                    scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
                } else {
                    fp->setFocusedPopup(popups[cur + 1].id);
                    // Focus moves between popups — main terminal stays unfocused
                    scriptEngine_.notifyFocusedPopupChanged(fp->id(), popups[cur + 1].id);
                }
            }
            // Mark pane dirty so cursor position updates in the pane texture
            renderThread_->pending().dirtyPanes.insert(fp->id());
            setNeedsRedraw();
        },
        [&](const Action::ShowScrollback&) {
            Terminal* term = dynamic_cast<Terminal*>(activeTerm());
            Tab* tab = activeTab();
            if (!term || !tab) return;

            // Write scrollback to temp file
            std::string content = term->serializeScrollback();
            char tmpPath[] = "/tmp/mb-scrollback-XXXXXX";
            int tmpFd = mkstemp(tmpPath);
            if (tmpFd < 0) return;
            const char* ptr = content.data();
            size_t remaining = content.size();
            while (remaining > 0) {
                ssize_t written = ::write(tmpFd, ptr, remaining);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    ::close(tmpFd);
                    ::unlink(tmpPath);
                    return;
                }
                ptr += written;
                remaining -= written;
            }
            ::close(tmpFd);

            // Spawn pager as overlay terminal
            TerminalOptions opts = tabManager_->terminalOptions();
            opts.command = "less -+F -R " + std::string(tmpPath) + "; rm -f " + std::string(tmpPath);
            opts.scrollbackLines = 0;

            TerminalCallbacks cbs;
            cbs.event = [this](TerminalEmulator*, int, void*) {
                // Overlay uses a single PaneRenderPrivate (overlayRenderPrivate_),
                // dirty is set via a general "overlay needs redraw" path.
                setNeedsRedraw();
            };

            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [this, tab](Terminal* t) {
                // Defer cleanup — we're called from Terminal::readFromFD.
                int fd = t->masterFD();
                eventLoop_->addTimer(0, false, [this, tab, fd]() {
                    std::lock_guard<std::mutex> plk(renderThread_->mutex());
                    tabManager_->removePtyPoll(fd);
                    auto& tabsVec = tabManager_->tabs();
                    for (int ti = 0; ti < static_cast<int>(tabsVec.size()); ++ti) {
                        if (tabsVec[ti].get() == tab) {
                            scriptEngine_.notifyOverlayDestroyed(ti);
                            break;
                        }
                    }
                    tab->popOverlay();
                    // Clear stale pointer in renderThread_->renderState() immediately — the
                    // render thread may wake before the next applyPendingMutations()
                    // rebuilds the shadow copy via buildRenderFrameState().
                    renderThread_->renderState().hasOverlay = false;
                    renderThread_->renderState().overlay = nullptr;
                    if (renderEngine_) renderEngine_->clearOverlayFromFrameState();
                    renderThread_->pending().structuralOps.push_back(PendingMutations::DestroyOverlayState{});
                    if (inputController_) inputController_->refreshPointerShape();
                    setNeedsRedraw();
                });
            };
            pcbs.quit = [this]() { quit(); };
            {
                int tabIdx = tabManager_->activeTabIdx();
                pcbs.shouldFilterOutput = [this, tabIdx]() {
                    return scriptEngine_.hasOverlayOutputFilters(tabIdx);
                };
                pcbs.filterOutput = [this, tabIdx](std::string& data) {
                    scriptEngine_.filterOverlayOutput(tabIdx, data);
                };
                pcbs.shouldFilterInput = [this, tabIdx]() {
                    return scriptEngine_.hasOverlayInputFilters(tabIdx);
                };
                pcbs.filterInput = [this, tabIdx](std::string& data) {
                    scriptEngine_.filterOverlayInput(tabIdx, data);
                };
            }

            auto overlay = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));

            // Size overlay before init so the PTY is created with the correct
            // dimensions. The child process reads the initial PTY size before
            // any SIGWINCH can arrive.
            float usableW = std::max(0.0f, static_cast<float>(fbWidth_) - padLeft_ - padRight_);
            float usableH = std::max(0.0f, static_cast<float>(fbHeight_) - padTop_ - padBottom_);
            int cols = std::max(1, static_cast<int>(usableW / charWidth_));
            int rows = std::max(1, static_cast<int>(usableH / lineHeight_));
            overlay->resize(cols, rows);

            if (!overlay->init(opts)) return;

            tabManager_->addPtyPoll(overlay->masterFD(), overlay.get());
            tab->pushOverlay(std::move(overlay));
            scriptEngine_.notifyOverlayCreated(tabManager_->activeTabIdx());
            if (inputController_) inputController_->refreshPointerShape();  // overlay may want a different cursor
            setNeedsRedraw();
        },
        [&](const Action::ReloadConfig&) { if (configLoader_) configLoader_->reloadNow(); },
        [&](const Action::MouseSelection&) { /* TODO: wire up in mouse binding phase */ },
        [&](const Action::OpenHyperlink&) { /* TODO: wire up in mouse binding phase */ },
        [&](const Action::PasteSelection&) {
            Terminal* term = activeTerm();
            if (!term || !window_) return;
            std::string text = window_->getPrimarySelection();
            if (text.empty()) text = window_->getClipboard();
            if (!text.empty()) term->pasteText(text);
        },
        [&](const Action::ScriptAction& a) {
            if (!scriptEngine_.isActionRegistered(a.name)) {
                spdlog::debug("ScriptAction '{}' not registered, ignoring", a.name);
                return;
            }
            // Notification to JS listeners happens via ActionRouter::dispatch
        },
    }, action);
}

