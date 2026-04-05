#include "PlatformDawn.h"
#include "Log.h"


void PlatformDawn::dispatchAction(const Action::Any& action)
{
    std::visit(overloaded {
        [&](const Action::NewTab&)  { createTab(); },
        [&](const Action::CloseTab&) { closeTab(activeTabIdx_); },
        [&](const Action::ActivateTabRelative& a) {
            int idx = activeTabIdx_ + a.delta;
            if (idx >= 0 && idx < static_cast<int>(tabs_.size())) {
                clearDividers(activeTab());
                releaseTabTextures(activeTab());
                activeTabIdx_ = idx;
                refreshDividers(activeTab());
                tabBarDirty_ = true;
                needsRedraw_ = true;
            }
        },
        [&](const Action::ActivateTab& a) {
            if (a.index >= 0 && a.index < static_cast<int>(tabs_.size())) {
                clearDividers(activeTab());
                releaseTabTextures(activeTab());
                activeTabIdx_ = a.index;
                refreshDividers(activeTab());
                tabBarDirty_ = true;
                needsRedraw_ = true;
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
            int tabIdx = activeTabIdx_;
            int prevId = layout->focusedPaneId();
            spawnTerminalForPane(newPane, tabIdx);
            resizeAllPanesInTab(tab);
            layout->setFocusedPane(newId);
            notifyPaneFocusChange(tab, prevId, newId);
            updateTabTitleFromFocusedPane(activeTabIdx_);
        },
        [&](const Action::ClosePane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            if (layout->panes().size() <= 1) return; // keep last pane
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            int paneId = fp->id();

            // Stop PTY poll and release render state
            if (auto* t = dynamic_cast<Terminal*>(fp->activeTerm())) {
                removePtyPoll(t->masterFD());
            }
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) {
                if (it->second.heldTexture)
                    pendingTabBarRelease_.push_back(it->second.heldTexture);
                for (auto* tx : it->second.pendingRelease)
                    pendingTabBarRelease_.push_back(tx);
                paneRenderStates_.erase(it);
            }

            layout->removePane(paneId);
            resizeAllPanesInTab(tab);
            notifyPaneFocusChange(tab, -1, layout->focusedPaneId());
            updateTabTitleFromFocusedPane(activeTabIdx_);
        },
        [&](const Action::ZoomPane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            layout->zoomPane(fp->id());
            resizeAllPanesInTab(tab);
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
                notifyPaneFocusChange(tab, prev, panes[next]->id());
                updateTabTitleFromFocusedPane(activeTabIdx_);
                needsRedraw_ = true;
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
                notifyPaneFocusChange(tab, prev, targetId);
                updateTabTitleFromFocusedPane(activeTabIdx_);
                needsRedraw_ = true;
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
                resizeAllPanesInTab(tab);
        },
        [&](const Action::Copy&) {
            Terminal* term = activeTerm();
            if (term && term->hasSelection()) {
                std::string text = term->selectedText();
                if (!text.empty())
                    glfwSetClipboardString(glfwWindow_, text.c_str());
            }
        },
        [&](const Action::Paste&) {
            Terminal* term = activeTerm();
            const char* clip = glfwGetClipboardString(glfwWindow_);
            if (term && clip && clip[0])
                term->pasteText(std::string(clip));
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
            if (tab) tab->popOverlay();
        },
        [&](const Action::IncreaseFontSize&) { adjustFontSize(1.0f);  },
        [&](const Action::DecreaseFontSize&) { adjustFontSize(-1.0f); },
        [&](const Action::ResetFontSize&)    { adjustFontSize(0.0f);  },
    }, action);
}

