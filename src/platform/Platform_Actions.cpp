#include "PlatformDawn.h"
#include "PlatformUtils.h"
#include "Config.h"
#include <quickjs.h>
#include <unistd.h>

namespace {
const char* directionName(Action::Direction d) {
    switch (d) {
    case Action::Direction::Left:  return "left";
    case Action::Direction::Right: return "right";
    case Action::Direction::Up:    return "up";
    case Action::Direction::Down:  return "down";
    case Action::Direction::Next:  return "next";
    case Action::Direction::Prev:  return "prev";
    }
    return "unknown";
}
} // namespace

void PlatformDawn::dispatchAction(const Action::Any& action)
{
    if (actionRouter_) actionRouter_->dispatch(action);
    else executeAction(action);
}

void PlatformDawn::executeAction(const Action::Any& action)
{
    // JS-owned actions: default-ui.js is the sole implementation. Missing
    // handler is an error — the controller is required to register for every
    // action in this group.
    auto invokeOrLog = [&](const char* name,
                           const std::function<JSValue(JSContext*)>& args) {
        if (!scriptEngine_.invokeActionHandler(name, args))
            spdlog::error("no controller handler for action '{}'", name);
    };

    std::visit(overloaded {
        [&](const Action::NewTab&) { invokeOrLog("newTab", nullptr); },
        [&](const Action::CloseTab& a) {
            const int idx = a.index;
            invokeOrLog("closeTab", [idx](JSContext* ctx) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, idx));
                return o;
            });
        },
        [&](const Action::ActivateTabRelative& a) {
            const int delta = a.delta;
            invokeOrLog("activateTabRelative", [delta](JSContext* ctx) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "delta", JS_NewInt32(ctx, delta));
                return o;
            });
        },
        [&](const Action::ActivateTab& a) {
            const int idx = a.index;
            invokeOrLog("activateTab", [idx](JSContext* ctx) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, idx));
                return o;
            });
        },
        [&](const Action::SplitPane& a) {
            const char* dirStr = directionName(a.dir);
            invokeOrLog("splitPane", [dirStr](JSContext* ctx) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "dir", JS_NewString(ctx, dirStr));
                return o;
            });
        },
        [&](const Action::ClosePane&) { invokeOrLog("closePane", nullptr); },
        [&](const Action::ZoomPane&)  { invokeOrLog("zoomPane",  nullptr); },
        [&](const Action::FocusPane& a) {
            auto tab = activeTab();
            if (!tab || !tab->valid()) return;

            if (a.dir == Action::Direction::Next || a.dir == Action::Direction::Prev) {
                auto panes = tab->panes();
                if (panes.size() <= 1) return;
                int cur = -1;
                for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                    if (panes[i]->id() == tab->focusedPaneId()) { cur = i; break; }
                }
                if (cur < 0) return;
                int n = static_cast<int>(panes.size());
                int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                int next = ((cur + delta) % n + n) % n;
                int prev = tab->focusedPaneId();
                tab->setFocusedPane(panes[next]->id());
                tabManager_->notifyPaneFocusChange(*tab, prev, panes[next]->id());
                tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
                setNeedsRedraw();
                return;
            }

            Terminal* fp = tab->focusedPane();
            if (!fp) return;
            const PaneRect& r = fp->rect();
            // Step past the divider gap; +1 alone lands inside the divider
            // and paneAtPixel returns -1.
            const int step = tab->dividerPixels() + 1;
            int px = 0, py = 0;
            switch (a.dir) {
            case Action::Direction::Left:  px = r.x - step;       py = r.y + r.h / 2; break;
            case Action::Direction::Right: px = r.x + r.w + step; py = r.y + r.h / 2; break;
            case Action::Direction::Up:    px = r.x + r.w / 2;    py = r.y - step;    break;
            case Action::Direction::Down:  px = r.x + r.w / 2;    py = r.y + r.h + step; break;
            default: return;
            }
            int targetId = tab->paneAtPixel(px, py);
            if (targetId >= 0 && targetId != fp->id()) {
                int prev = tab->focusedPaneId();
                tab->setFocusedPane(targetId);
                tabManager_->notifyPaneFocusChange(*tab, prev, targetId);
                tabManager_->updateTabTitleFromFocusedPane(tabManager_->activeTabIdx());
                setNeedsRedraw();
            }
        },
        [&](const Action::AdjustPaneSize& a) {
            const char* dirStr = directionName(a.dir);
            const int amount = a.amount;
            invokeOrLog("adjustPaneSize", [dirStr, amount](JSContext* ctx) {
                JSValue o = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, o, "dir", JS_NewString(ctx, dirStr));
                JS_SetPropertyStr(ctx, o, "amount", JS_NewInt32(ctx, amount));
                return o;
            });
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
        [&](const Action::IncreaseFontSize&) { adjustFontSize(1.0f);  },
        [&](const Action::DecreaseFontSize&) { adjustFontSize(-1.0f); },
        [&](const Action::ResetFontSize&)    { adjustFontSize(0.0f);  },
        [&](const Action::ScrollToPrompt& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollToPrompt(a.direction, commandNavigationWrap_);
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
            std::string text = term->document().getTextFromLines(
                cmd->promptStartLineId, cmd->outputEndLineId,
                cmd->promptStartCol,    cmd->outputEndCol);
            if (!text.empty() && window_) window_->setClipboard(text);
        },
        [&](const Action::CopySelectedCommandOutput&) {
            Terminal* term = activeTerm();
            if (!term) return;
            auto id = term->selectedCommandId();
            if (!id) return;
            const auto* cmd = term->commandForId(*id);
            if (!cmd) return;
            std::string text = term->document().getTextFromLines(
                cmd->outputStartLineId, cmd->outputEndLineId,
                cmd->outputStartCol,    cmd->outputEndCol);
            if (!text.empty() && window_) window_->setClipboard(text);
        },
        [&](const Action::CopyDocument&) {
            Terminal* term = activeTerm();
            if (!term) return;
            std::string text = term->serializeScrollback();
            if (!text.empty() && window_) window_->setClipboard(text);
        },
        [&](const Action::FocusPopup&) {
            if (scriptEngine_.invokeActionHandler("focusPopup", nullptr)) return;
            auto tab = activeTab();
            if (!tab) return;
            Terminal* fp = tab->focusedPane();
            if (!fp || fp->popups().empty()) return;

            const auto& popups = fp->popups();
            const std::string& curFocus = fp->focusedPopupId();

            if (curFocus.empty()) {
                // No popup focused — focus first popup
                fp->setFocusedPopup(popups.front()->popupId());
                fp->focusEvent(false);
                scriptEngine_.notifyFocusedPopupChanged(fp->id(), popups.front()->popupId());
            } else {
                // Find current popup index, cycle to next or back to main
                int cur = -1;
                for (int i = 0; i < static_cast<int>(popups.size()); ++i) {
                    if (popups[i]->popupId() == curFocus) { cur = i; break; }
                }
                if (cur < 0 || cur + 1 >= static_cast<int>(popups.size())) {
                    // Last popup or not found — back to main terminal
                    fp->clearFocusedPopup();
                    fp->focusEvent(true);
                    scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
                } else {
                    fp->setFocusedPopup(popups[cur + 1]->popupId());
                    // Focus moves between popups — main terminal stays unfocused
                    scriptEngine_.notifyFocusedPopupChanged(fp->id(), popups[cur + 1]->popupId());
                }
            }
            // Mark pane dirty so cursor position updates in the pane texture
            renderThread_->pending().dirtyPanes.insert(fp->id());
            setNeedsRedraw();
        },
        [&](const Action::ShowScrollback&) {
            // Spawn a `less` pager as a Stack sibling of the active tab's
            // content. When the user quits `less`, the pager Terminal's
            // shell-exit path fires → TabManager::killTerminal extracts it →
            // JS terminalExited handler calls `mb.layout.removeNode` → the
            // Stack's activeChild auto-retargets to the content Container
            // (first remaining child), so the panes come back. No overlay-
            // specific machinery — the pager IS just another pane, attached
            // at the tab's Stack instead of under the content Container.
            Terminal* term = dynamic_cast<Terminal*>(activeTerm());
            auto tab = activeTab();
            if (!term || !tab) return;

            // Write scrollback to temp file.
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

            TerminalOptions opts = tabManager_->terminalOptions();
            opts.command = "less -+F -R " + std::string(tmpPath) + "; rm -f " + std::string(tmpPath);
            opts.scrollbackLines = 0;

            // Allocate paneId + tree node for the pager. Register in the
            // engine's paneId index so pane-by-id lookups work.
            int pagerId = scriptEngine_.allocatePaneId();
            LayoutTree& tree = scriptEngine_.layoutTree();
            Uuid pagerNode = tree.createTerminal();
            scriptEngine_.registerPaneSlot(pagerId, pagerNode);

            TerminalCallbacks cbs;
            cbs.event = [this](TerminalEmulator*, int, void*) {
                setNeedsRedraw();
            };
            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [this](Terminal* t) {
                // Queue into the render-thread exit drain — same path as any
                // other pane. TabManager::terminalExited → killTerminal fires
                // the JS `terminalExited` event, and the controller removes
                // the tree node. Stack's activeChild auto-retargets to the
                // remaining content Container, restoring the pane view.
                if (renderThread_) renderThread_->enqueueTerminalExit(t);
            };
            pcbs.quit = [this]() { quit(); };
            Script::Engine* scriptEngine = &scriptEngine_;
            pcbs.shouldFilterOutput = [scriptEngine, pagerId]() {
                return scriptEngine->hasPaneOutputFilters(pagerId);
            };
            pcbs.filterOutput = [scriptEngine, pagerId](std::string& data) {
                scriptEngine->filterPaneOutput(pagerId, data);
            };
            pcbs.shouldFilterInput = [scriptEngine, pagerId]() {
                return scriptEngine->hasPaneInputFilters(pagerId);
            };
            pcbs.filterInput = [scriptEngine, pagerId](std::string& data) {
                scriptEngine->filterPaneInput(pagerId, data);
            };

            auto pager = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
            pager->setId(pagerId);
            pager->setNodeId(pagerNode);

            float usableW = std::max(0.0f, static_cast<float>(fbWidth_) - padLeft_ - padRight_);
            float usableH = std::max(0.0f, static_cast<float>(fbHeight_) - padTop_ - padBottom_);
            int cols = std::max(1, static_cast<int>(usableW / charWidth_));
            int rows = std::max(1, static_cast<int>(usableH / lineHeight_));
            pager->resize(cols, rows);

            if (!pager->init(opts)) return;

            Terminal* pagerPtr = pager.get();
            int pagerFD = pager->masterFD();
            scriptEngine_.insertTerminal(pagerNode, std::move(pager));

            // Attach as a sibling under the tab's Stack. The Stack
            // auto-targets activeChild to this new sibling via setActiveChild.
            Uuid tabStack = tab->subtreeRoot();
            tree.appendChild(tabStack, ChildSlot{pagerNode, /*stretch=*/1});
            tree.setActiveChild(tabStack, pagerNode);

            tabManager_->addPtyPoll(pagerFD, pagerPtr);
            // Move focus to the pager so input routes to it.
            scriptEngine_.setFocusedTerminalNodeId(pagerNode);
            scriptEngine_.notifyPaneCreated(tabManager_->activeTabIdx(), pagerId);

            if (inputController_) inputController_->refreshPointerShape();
            setNeedsRedraw();
        },
        [&](const Action::ReloadConfig&) { if (configLoader_) configLoader_->reloadNow(); },
        [&](const Action::MouseSelection&) { /* TODO: wire up in mouse binding phase */ },
        [&](const Action::OpenHyperlink&) { /* TODO: wire up in mouse binding phase */ },
        [&](const Action::SelectCommand&) { /* mouse-bound only; no-op via generic dispatch */ },
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

