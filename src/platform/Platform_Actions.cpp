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
    actionRouter_->dispatch(action);
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
            if (!tab) return;

            if (a.dir == Action::Direction::Next || a.dir == Action::Direction::Prev) {
                auto panes = scriptEngine_.panesInSubtree(*tab);
                if (panes.size() <= 1) return;
                int cur = -1;
                Uuid focusedId = scriptEngine_.focusedPaneInSubtree(*tab);
                for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                    if (panes[i]->nodeId() == focusedId) { cur = i; break; }
                }
                if (cur < 0) return;
                int n = static_cast<int>(panes.size());
                int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                int next = ((cur + delta) % n + n) % n;
                Uuid prev = focusedId;
                Uuid nextId = panes[next]->nodeId();
                scriptEngine_.setFocusedTerminalNodeId(nextId);
                notifyPaneFocusChange(*tab, prev, nextId);
                tabBarDirty_ = true;
                updateWindowTitle();
                setNeedsRedraw();
                return;
            }

            Terminal* fp = scriptEngine_.focusedTerminalInSubtree(*tab);
            if (!fp) return;
            Uuid targetId;
            switch (a.dir) {
            case Action::Direction::Left:  targetId = scriptEngine_.paneLeftOf(*tab, fp->nodeId());  break;
            case Action::Direction::Right: targetId = scriptEngine_.paneRightOf(*tab, fp->nodeId()); break;
            case Action::Direction::Up:    targetId = scriptEngine_.paneAboveOf(*tab, fp->nodeId()); break;
            case Action::Direction::Down:  targetId = scriptEngine_.paneBelowOf(*tab, fp->nodeId()); break;
            default: return;
            }
            if (!targetId.isNil() && targetId != fp->nodeId()) {
                Uuid prev = scriptEngine_.focusedPaneInSubtree(*tab);
                scriptEngine_.setFocusedTerminalNodeId(targetId);
                notifyPaneFocusChange(*tab, prev, targetId);
                tabBarDirty_ = true;
                updateWindowTitle();
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
        [&](const Action::MoveTab& a) {
            // Reorder the active tab among its siblings under the root stack.
            // activeChild stores a Uuid, so swap preserves focus on the moved
            // tab automatically. moveChild marks the tree dirty; the next
            // frame's runLayoutIfDirty cascades resize.
            auto& tree = scriptEngine_.layoutTree();
            Uuid tab = scriptEngine_.activeTabSubtreeRoot();
            if (tab.isNil()) return;
            Node* tabNode = tree.node(tab);
            if (!tabNode) return;
            Uuid rootStack = tabNode->parent;
            if (rootStack.isNil()) return;
            if (tree.moveChild(rootStack, tab, a.delta)) {
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        },
        [&](const Action::SwapPane& a) {
            // Swap focused pane with the pane in the requested direction.
            // Spatial L/R/U/D use the same neighbor lookup as FocusPane;
            // Next/Prev wraps through the tab's leaves in tree-traversal
            // order. swapLeaves preserves slot weights at each position so
            // the layout doesn't change — only the leaves move. Focus
            // follows the same Uuid (now in a different visual slot).
            auto tab = activeTab();
            if (!tab) return;
            Uuid focused = scriptEngine_.focusedPaneInSubtree(*tab);
            if (focused.isNil()) return;
            Uuid target;
            switch (a.dir) {
            case Action::Direction::Left:  target = scriptEngine_.paneLeftOf(*tab, focused);  break;
            case Action::Direction::Right: target = scriptEngine_.paneRightOf(*tab, focused); break;
            case Action::Direction::Up:    target = scriptEngine_.paneAboveOf(*tab, focused); break;
            case Action::Direction::Down:  target = scriptEngine_.paneBelowOf(*tab, focused); break;
            case Action::Direction::Next:
            case Action::Direction::Prev: {
                auto panes = scriptEngine_.panesInSubtree(*tab);
                if (panes.size() < 2) return;
                int cur = -1;
                for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                    if (panes[i]->nodeId() == focused) { cur = i; break; }
                }
                if (cur < 0) return;
                int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                int n = static_cast<int>(panes.size());
                int idx = ((cur + delta) % n + n) % n;
                target = panes[idx]->nodeId();
                break;
            }
            }
            if (target.isNil() || target == focused) return;
            if (scriptEngine_.layoutTree().swapLeaves(focused, target)) {
                // dividersDirty_ (member) reaches renderState_ via the merge
                // inside buildRenderFrameState. pending_.dividersDirty set by
                // refreshDividers (called later, from runLayoutIfDirty) does
                // not — it's wiped by pending_.clear() before the next tick.
                // Without this, the renderer skips its VB update path and
                // dividers render with stale geometry after the swap.
                dividersDirty_ = true;
                setNeedsRedraw();
            }
        },
        [&](const Action::RotatePanes& a) {
            // Rotate every leaf in the active tab through tree-traversal
            // order. delta=+1 ("clockwise") → each leaf advances one slot
            // forward, the last leaf wraps to the front. delta=-1 reverses.
            // Implementation: pick one leaf and bubble it across n-1 slots
            // via successive swapLeaves calls. The same leaf is always one
            // half of the swap, so it carries forward through every step
            // (snapshotting both sides instead would only do single-step
            // adjacent shifts and produce the opposite rotation).
            auto tab = activeTab();
            if (!tab) return;
            if (a.delta == 0) return;
            auto& tree = scriptEngine_.layoutTree();
            for (int s = 0; s < std::abs(a.delta); ++s) {
                auto p = scriptEngine_.panesInSubtree(*tab);
                int m = static_cast<int>(p.size());
                if (m < 2) break;
                if (a.delta > 0) {
                    // Bubble the last leaf to the front.
                    Uuid bubble = p.back()->nodeId();
                    for (int i = m - 2; i >= 0; --i) {
                        tree.swapLeaves(bubble, p[i]->nodeId());
                    }
                } else {
                    // Bubble the first leaf to the back.
                    Uuid bubble = p.front()->nodeId();
                    for (int i = 1; i < m; ++i) {
                        tree.swapLeaves(bubble, p[i]->nodeId());
                    }
                }
            }
            // See SwapPane comment: pending_.dividersDirty set by the eventual
            // refreshDividers doesn't propagate to renderState_; the member
            // flag does (merged in buildRenderFrameState).
            dividersDirty_ = true;
            setNeedsRedraw();
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
            Terminal* fp = scriptEngine_.focusedTerminalInSubtree(*tab);
            if (!fp) return;

            const auto& popups = fp->popups();
            const std::string& curFocus = fp->focusedPopupId();
            const uint64_t curEmbedded = fp->focusedEmbeddedLineId();

            // Embeddeds cycled in ascending lineId order (creation order).
            // unordered_map iteration is unstable so gather + sort.
            std::vector<uint64_t> embeddedIds;
            embeddedIds.reserve(fp->embeddeds().size());
            for (const auto& [lineId, em] : fp->embeddeds()) embeddedIds.push_back(lineId);
            std::sort(embeddedIds.begin(), embeddedIds.end());

            if (popups.empty() && embeddedIds.empty()) return;

            auto focusPopupIdx = [&](size_t i) {
                const std::string& pid = popups[i]->popupId();
                fp->clearFocusedEmbedded();
                fp->setFocusedPopup(pid);
                fp->focusEvent(false);
                scriptEngine_.notifyFocusedPopupChanged(fp->id(), pid);
            };
            auto focusEmbeddedIdx = [&](size_t i) {
                fp->clearFocusedPopup();
                fp->setFocusedEmbeddedLineId(embeddedIds[i]);
                fp->focusEvent(false);
                scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
            };
            auto focusPane = [&]() {
                fp->clearFocusedPopup();
                fp->clearFocusedEmbedded();
                fp->focusEvent(true);
                scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
            };

            if (!curFocus.empty()) {
                // Currently in a popup — next popup, or first embedded, or pane.
                int cur = -1;
                for (int i = 0; i < static_cast<int>(popups.size()); ++i) {
                    if (popups[i]->popupId() == curFocus) { cur = i; break; }
                }
                if (cur >= 0 && cur + 1 < static_cast<int>(popups.size()))
                    focusPopupIdx(cur + 1);
                else if (!embeddedIds.empty())
                    focusEmbeddedIdx(0);
                else
                    focusPane();
            } else if (curEmbedded != 0) {
                // Currently in an embedded — next embedded, or pane.
                size_t cur = embeddedIds.size();
                for (size_t i = 0; i < embeddedIds.size(); ++i) {
                    if (embeddedIds[i] == curEmbedded) { cur = i; break; }
                }
                if (cur + 1 < embeddedIds.size())
                    focusEmbeddedIdx(cur + 1);
                else
                    focusPane();
            } else {
                // Pane focused — first popup, else first embedded.
                if (!popups.empty()) focusPopupIdx(0);
                else                 focusEmbeddedIdx(0);
            }

            // Mark pane dirty so cursor position updates in the pane texture
            renderThread_->pending().dirtyPanes.insert(fp->id());
            setNeedsRedraw();
        },
        [&](const Action::ShowScrollback&) {
            // Spawn a `less` pager as a Stack sibling of the active tab's
            // content. When the user quits `less`, the pager Terminal's
            // shell-exit path fires → killTerminal extracts it → JS
            // terminalExited handler calls `mb.layout.removeNode` → the
            // Stack's activeChild auto-retargets to the content Container
            // (first remaining child), so the panes come back. No overlay-
            // specific machinery — the pager IS just another pane, attached
            // at the tab's Stack instead of under the content Container.
            Terminal* term = dynamic_cast<Terminal*>(activeTerm());
            auto tab = activeTab();
            if (!term || !tab) return;

            // Remember which Terminal had focus before the pager takes over
            // so we can restore it when `less` exits. Without this, the
            // post-removal fallback in removeNode just picks the first
            // pane, which may not be the one the user was on.
            Uuid prevFocus = scriptEngine_.focusedTerminalNodeId();

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

            TerminalOptions opts = terminalOptions();
            opts.command = "less -+F -R " + std::string(tmpPath) + "; rm -f " + std::string(tmpPath);
            opts.scrollbackLines = 0;

            // Allocate a tree node for the pager. Uuid is the sole identity.
            LayoutTree& tree = scriptEngine_.layoutTree();
            Uuid pagerNode = tree.createTerminal();

            TerminalCallbacks cbs;
            cbs.event = [this](TerminalEmulator*, int, void*) {
                setNeedsRedraw();
            };
            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [this, prevFocus](Terminal* t) {
                // Restore focus to the previously-focused Terminal (if it
                // still exists in the tree) before the removal cascade.
                if (!prevFocus.isNil() &&
                    scriptEngine_.layoutTree().node(prevFocus)) {
                    scriptEngine_.setFocusedTerminalNodeId(prevFocus);
                }
                if (renderThread_) renderThread_->enqueueTerminalExit(t);
            };
            pcbs.quit = [this]() { quit(); };
            Script::Engine* scriptEngine = &scriptEngine_;
            pcbs.shouldFilterOutput = [scriptEngine, pagerNode]() {
                return scriptEngine->hasPaneOutputFilters(pagerNode);
            };
            pcbs.filterOutput = [scriptEngine, pagerNode](std::string& data) {
                scriptEngine->filterPaneOutput(pagerNode, data);
            };
            pcbs.shouldFilterInput = [scriptEngine, pagerNode]() {
                return scriptEngine->hasPaneInputFilters(pagerNode);
            };
            pcbs.filterInput = [scriptEngine, pagerNode](std::string& data) {
                scriptEngine->filterPaneInput(pagerNode, data);
            };

            auto pager = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
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
            Uuid tabStack = *tab;
            tree.appendChild(tabStack, ChildSlot{pagerNode, /*stretch=*/1});
            tree.setActiveChild(tabStack, pagerNode);

            addPtyPoll(pagerFD, pagerPtr);
            // Move focus to the pager so input routes to it.
            scriptEngine_.setFocusedTerminalNodeId(pagerNode);
            scriptEngine_.notifyPaneCreated(scriptEngine_.activeTabSubtreeRoot(), pagerNode);

            // The pager's Terminal has no rect yet — computeRects populates
            // it from the tree shape (now that the node is attached and
            // activeChild points at it). Without this the pager renders at
            // {0,0,0,0}, the old pane's framebuffer stays frozen on screen,
            // and mouse hit-testing against the pager fails.
            resizeAllPanesInTab(*tab);

            if (inputController_) inputController_->refreshPointerShape();
            setNeedsRedraw();
        },
        [&](const Action::ReloadConfig&) { if (configLoader_) configLoader_->reloadNow(); },
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

