#include "Config.h"
#include "PlatformDawn.h"
#include "PlatformUtils.h"
#include <quickjs.h>

namespace {
const char *directionName(Action::Direction d)
{
    switch (d) {
        case Action::Direction::Left: return "left";
        case Action::Direction::Right: return "right";
        case Action::Direction::Up: return "up";
        case Action::Direction::Down: return "down";
        case Action::Direction::Next: return "next";
        case Action::Direction::Prev: return "prev";
    }
    return "unknown";
}
} // namespace

void PlatformDawn::dispatchAction(const Action::Any &action)
{
    actionRouter_->dispatch(action);
}

void PlatformDawn::executeAction(const Action::Any &action)
{
    // JS-owned actions: default-ui.js is the sole implementation. Missing
    // handler is an error — the controller is required to register for every
    // action in this group.
    auto invokeOrLog = [&](const char *name,
                           const std::function<JSValue(JSContext *)> &args)
    {
        if (!scriptEngine_.invokeActionHandler(name, args)) {
            spdlog::error("no controller handler for action '{}'", name);
        }
    };

    std::visit(overloaded {
                   [&](const Action::NewTab &)
                   {
                       invokeOrLog("newTab", nullptr);
                   },
                   [&](const Action::CloseTab &a)
                   {
                       const std::string targetStr = a.target.isNil() ? std::string {} : a.target.toString();
                       const int idx               = a.index;
                       invokeOrLog("closeTab", [targetStr, idx](JSContext *ctx)
                                   {
                                       JSValue o = JS_NewObject(ctx);
                                       if (!targetStr.empty()) {
                                           JS_SetPropertyStr(ctx, o, "target", JS_NewStringLen(ctx, targetStr.data(), targetStr.size()));
                                       }
                                       JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, idx));
                                       return o;
                                   });
                   },
                   [&](const Action::ActivateTabRelative &a)
                   {
                       const std::string stackStr = a.stack.isNil() ? std::string {} : a.stack.toString();
                       const int delta            = a.delta;
                       invokeOrLog("activateTabRelative", [stackStr, delta](JSContext *ctx)
                                   {
                                       JSValue o = JS_NewObject(ctx);
                                       if (!stackStr.empty()) {
                                           JS_SetPropertyStr(ctx, o, "stack", JS_NewStringLen(ctx, stackStr.data(), stackStr.size()));
                                       }
                                       JS_SetPropertyStr(ctx, o, "delta", JS_NewInt32(ctx, delta));
                                       return o;
                                   });
                   },
                   [&](const Action::ActivateTab &a)
                   {
                       const std::string targetStr = a.target.isNil() ? std::string {} : a.target.toString();
                       const int idx               = a.index;
                       invokeOrLog("activateTab", [targetStr, idx](JSContext *ctx)
                                   {
                                       JSValue o = JS_NewObject(ctx);
                                       if (!targetStr.empty()) {
                                           JS_SetPropertyStr(ctx, o, "target", JS_NewStringLen(ctx, targetStr.data(), targetStr.size()));
                                       }
                                       JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, idx));
                                       return o;
                                   });
                   },
                   [&](const Action::SplitPane &a)
                   {
                       const char *dirStr = directionName(a.dir);
                       invokeOrLog("splitPane", [dirStr](JSContext *ctx)
                                   {
                                       JSValue o = JS_NewObject(ctx);
                                       JS_SetPropertyStr(ctx, o, "dir", JS_NewString(ctx, dirStr));
                                       return o;
                                   });
                   },
                   [&](const Action::ClosePane &)
                   {
                       invokeOrLog("closePane", nullptr);
                   },
                   [&](const Action::ZoomPane &)
                   {
                       invokeOrLog("zoomPane", nullptr);
                   },
                   [&](const Action::FocusPane &a)
                   {
                       auto tab = activeTab();
                       if (!tab) {
                           return;
                       }

                       if (a.dir == Action::Direction::Next || a.dir == Action::Direction::Prev) {
                           // Cycle through CURRENTLY VISIBLE leaves only —
                           // hidden sub-tab panes (siblings of the active
                           // sub-stack child) must not be focus targets.
                           auto panes = scriptEngine_.activePanesInSubtree(*tab);
                           if (panes.size() <= 1) {
                               return;
                           }
                           int cur        = -1;
                           Uuid focusedId = scriptEngine_.focusedPaneInSubtree(*tab);
                           for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                               if (panes[i]->nodeId() == focusedId) {
                                   cur = i;
                                   break;
                               }
                           }
                           if (cur < 0) {
                               return;
                           }
                           int n       = static_cast<int>(panes.size());
                           int delta   = (a.dir == Action::Direction::Next) ? 1 : -1;
                           int next    = ((cur + delta) % n + n) % n;
                           Uuid prev   = focusedId;
                           Uuid nextId = panes[next]->nodeId();
                           scriptEngine_.setFocusedTerminalNodeId(nextId);
                           notifyPaneFocusChange(*tab, prev, nextId);
                           tabBarDirty_ = true;
                           updateWindowTitle();
                           setNeedsRedraw();
                           return;
                       }

                       Terminal *fp = scriptEngine_.focusedTerminalInSubtree(*tab);
                       if (!fp) {
                           return;
                       }
                       Uuid targetId;
                       switch (a.dir) {
                           case Action::Direction::Left: targetId = scriptEngine_.paneLeftOf(*tab, fp->nodeId()); break;
                           case Action::Direction::Right: targetId = scriptEngine_.paneRightOf(*tab, fp->nodeId()); break;
                           case Action::Direction::Up: targetId = scriptEngine_.paneAboveOf(*tab, fp->nodeId()); break;
                           case Action::Direction::Down: targetId = scriptEngine_.paneBelowOf(*tab, fp->nodeId()); break;
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
                   [&](const Action::AdjustPaneSize &a)
                   {
                       const char *dirStr = directionName(a.dir);
                       const int amount   = a.amount;
                       invokeOrLog("adjustPaneSize", [dirStr, amount](JSContext *ctx)
                                   {
                                       JSValue o = JS_NewObject(ctx);
                                       JS_SetPropertyStr(ctx, o, "dir", JS_NewString(ctx, dirStr));
                                       JS_SetPropertyStr(ctx, o, "amount", JS_NewInt32(ctx, amount));
                                       return o;
                                   });
                   },
                   [&](const Action::MoveTab &a)
                   {
                       // Reorder the focused-context Stack-child within its
                       // parent Stack: if focus is in a sub-tab, this moves
                       // the sub-tab; if focus is in a top-level tab with no
                       // sub-bar, this moves the top-level tab. The Stack's
                       // activeChild stores a Uuid, so the moved child keeps
                       // activation through the reorder. moveChild marks the
                       // tree dirty; the next frame's runLayoutIfDirty
                       // cascades resize.
                       auto &tree = scriptEngine_.layoutTree();
                       Uuid focus = scriptEngine_.focusedTerminalNodeId();
                       Uuid target;
                       Uuid stackId;
                       if (!focus.isNil()) {
                           // Walk up from focused pane to find the innermost
                           // Stack-child + its parent Stack.
                           const Node *cur = tree.node(focus);
                           while (cur) {
                               if (cur->parent.isNil()) {
                                   break;
                               }
                               const Node *p = tree.node(cur->parent);
                               if (p && std::holds_alternative<StackData>(p->data)) {
                                   target  = cur->id;
                                   stackId = cur->parent;
                                   break;
                               }
                               cur = p;
                           }
                       }
                       if (target.isNil()) {
                           // No focus or no enclosing Stack found — fall back
                           // to active top-level tab in the root stack.
                           Uuid activeTop = scriptEngine_.activeTabSubtreeRoot();
                           if (activeTop.isNil()) {
                               return;
                           }
                           Node *topNode = tree.node(activeTop);
                           if (!topNode || topNode->parent.isNil()) {
                               return;
                           }
                           target  = activeTop;
                           stackId = topNode->parent;
                       }
                       if (tree.moveChild(stackId, target, a.delta)) {
                           tabBarDirty_ = true;
                           setNeedsRedraw();
                       }
                   },
                   [&](const Action::SwapPane &a)
                   {
                       // Swap focused pane with the pane in the requested direction.
                       // Spatial L/R/U/D use the same neighbor lookup as FocusPane;
                       // Next/Prev wraps through the tab's leaves in tree-traversal
                       // order. swapLeaves preserves slot weights at each position so
                       // the layout doesn't change — only the leaves move. Focus
                       // follows the same Uuid (now in a different visual slot).
                       auto tab = activeTab();
                       if (!tab) {
                           return;
                       }
                       Uuid focused = scriptEngine_.focusedPaneInSubtree(*tab);
                       if (focused.isNil()) {
                           return;
                       }
                       Uuid target;
                       switch (a.dir) {
                           case Action::Direction::Left: target = scriptEngine_.paneLeftOf(*tab, focused); break;
                           case Action::Direction::Right: target = scriptEngine_.paneRightOf(*tab, focused); break;
                           case Action::Direction::Up: target = scriptEngine_.paneAboveOf(*tab, focused); break;
                           case Action::Direction::Down: target = scriptEngine_.paneBelowOf(*tab, focused); break;
                           case Action::Direction::Next:
                           case Action::Direction::Prev: {
                               // Visible-only cycling for SwapPane too —
                               // swapping with a hidden sub-tab leaf would
                               // teleport panes invisibly.
                               auto panes = scriptEngine_.activePanesInSubtree(*tab);
                               if (panes.size() < 2) {
                                   return;
                               }
                               int cur = -1;
                               for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                                   if (panes[i]->nodeId() == focused) {
                                       cur = i;
                                       break;
                                   }
                               }
                               if (cur < 0) {
                                   return;
                               }
                               int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                               int n     = static_cast<int>(panes.size());
                               int idx   = ((cur + delta) % n + n) % n;
                               target    = panes[idx]->nodeId();
                               break;
                           }
                       }
                       if (target.isNil() || target == focused) {
                           return;
                       }
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
                   [&](const Action::RotatePanes &a)
                   {
                       // Rotate every leaf in the active tab through tree-traversal
                       // order. delta=+1 ("clockwise") → each leaf advances one slot
                       // forward, the last leaf wraps to the front. delta=-1 reverses.
                       // Implementation: pick one leaf and bubble it across n-1 slots
                       // via successive swapLeaves calls. The same leaf is always one
                       // half of the swap, so it carries forward through every step
                       // (snapshotting both sides instead would only do single-step
                       // adjacent shifts and produce the opposite rotation).
                       auto tab = activeTab();
                       if (!tab) {
                           return;
                       }
                       if (a.delta == 0) {
                           return;
                       }
                       auto &tree = scriptEngine_.layoutTree();
                       for (int s = 0; s < std::abs(a.delta); ++s) {
                           // Rotate only currently-visible leaves —
                           // sub-tab hidden panes would otherwise get
                           // shuffled into visible slots and visible
                           // panes vanish into hidden ones.
                           auto p = scriptEngine_.activePanesInSubtree(*tab);
                           int m  = static_cast<int>(p.size());
                           if (m < 2) {
                               break;
                           }
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
                   [&](const Action::Copy &)
                   {
                       Terminal *term = activeTerm();
                       if (!term) {
                           return;
                       }
                       std::string text;
                       {
                           std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                           if (term->hasSelection()) {
                               text = term->selectedText();
                           }
                       }
                       if (!text.empty() && window_) {
                           window_->setClipboard(text);
                       }
                   },
                   [&](const Action::Paste &)
                   {
                       Terminal *term = activeTerm();
                       if (!term || !window_) {
                           return;
                       }
                       // Fire-and-forget: same async pattern as middle-click PasteSelection.
                       // Re-resolves the terminal by nodeId at completion in case the
                       // pane was closed mid-flight.
                       const Uuid nodeId = term->nodeId();
                       window_->requestSelection(Window::SelectionSource::Clipboard,
                                                 [this, nodeId](std::optional<std::string> text)
                                                 {
                                                     if (!text || text->empty()) {
                                                         return;
                                                     }
                                                     if (auto *tt = scriptEngine_.terminal(nodeId)) {
                                                         tt->pasteText(*text);
                                                     }
                                                 });
                   },
                   [&](const Action::ScrollUp &a)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->scrollViewport(a.lines);
                       }
                   },
                   [&](const Action::ScrollDown &a)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->scrollViewport(-a.lines);
                       }
                   },
                   [&](const Action::ScrollToTop &)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->scrollViewport(std::numeric_limits<int>::max());
                       }
                   },
                   [&](const Action::ScrollToBottom &)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->resetViewport();
                       }
                   },
                   [&](const Action::IncreaseFontSize &)
                   {
                       adjustFontSize(1.0f);
                   },
                   [&](const Action::DecreaseFontSize &)
                   {
                       adjustFontSize(-1.0f);
                   },
                   [&](const Action::ResetFontSize &)
                   {
                       adjustFontSize(0.0f);
                   },
                   [&](const Action::ScrollToPrompt &a)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->scrollToPrompt(a.direction, commandNavigationWrap_);
                       }
                   },
                   [&](const Action::SelectCommandOutput &)
                   {
                       Terminal *term = activeTerm();
                       if (term) {
                           term->selectCommandOutput();
                       }
                   },
                   [&](const Action::CopyLastCommand &)
                   {
                       Terminal *term = activeTerm();
                       if (!term) {
                           return;
                       }
                       // Lock for command-record + document text extraction. The
                       // worker is concurrently writing both.
                       std::string text;
                       {
                           std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                           const auto *cmd = term->lastCommand();
                           if (!cmd || !cmd->complete) {
                               return;
                           }
                           // Extract from prompt start through output end.
                           text = term->document().getTextFromLines(
                               cmd->promptStartLineId,
                               cmd->outputEndLineId,
                               cmd->promptStartCol,
                               cmd->outputEndCol);
                       }
                       if (!text.empty() && window_) {
                           window_->setClipboard(text);
                       }
                   },
                   [&](const Action::CopySelectedCommandOutput &)
                   {
                       Terminal *term = activeTerm();
                       if (!term) {
                           return;
                       }
                       std::string text;
                       {
                           std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                           auto id = term->selectedCommandId();
                           if (!id) {
                               return;
                           }
                           const auto *cmd = term->commandForId(*id);
                           if (!cmd) {
                               return;
                           }
                           text = term->document().getTextFromLines(
                               cmd->outputStartLineId,
                               cmd->outputEndLineId,
                               cmd->outputStartCol,
                               cmd->outputEndCol);
                       }
                       if (!text.empty() && window_) {
                           window_->setClipboard(text);
                       }
                   },
                   [&](const Action::CopyDocument &)
                   {
                       Terminal *term = activeTerm();
                       if (!term) {
                           return;
                       }
                       std::string text = term->serializeScrollback();
                       if (!text.empty() && window_) {
                           window_->setClipboard(text);
                       }
                   },
                   [&](const Action::FocusPopup &)
                   {
                       if (scriptEngine_.invokeActionHandler("focusPopup", nullptr)) {
                           return;
                       }
                       auto tab = activeTab();
                       if (!tab) {
                           return;
                       }
                       Terminal *fp = scriptEngine_.focusedTerminalInSubtree(*tab);
                       if (!fp) {
                           return;
                       }

                       const auto &popups          = fp->popups();
                       const std::string &curFocus = fp->focusedPopupId();
                       const uint64_t curEmbedded  = fp->focusedEmbeddedLineId();

                       // Embeddeds cycled in ascending lineId order (creation order).
                       // unordered_map iteration is unstable so gather + sort.
                       // forEachEmbedded reads the lock-free embedded snapshot
                       // (atomically republished on every map mutation).
                       std::vector<uint64_t> embeddedIds;
                       fp->forEachEmbedded([&embeddedIds](uint64_t lineId, Terminal &)
                                           {
                                               embeddedIds.push_back(lineId);
                                           });
                       std::sort(embeddedIds.begin(), embeddedIds.end());

                       if (popups.empty() && embeddedIds.empty()) {
                           return;
                       }

                       auto focusPopupIdx = [&](size_t i)
                       {
                           const std::string &pid = popups[i]->popupId();
                           fp->clearFocusedEmbedded();
                           fp->setFocusedPopup(pid);
                           fp->focusEvent(false);
                           scriptEngine_.notifyFocusedPopupChanged(fp->id(), pid);
                       };
                       auto focusEmbeddedIdx = [&](size_t i)
                       {
                           fp->clearFocusedPopup();
                           fp->setFocusedEmbeddedLineId(embeddedIds[i]);
                           fp->focusEvent(false);
                           scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
                       };
                       auto focusPane = [&]()
                       {
                           fp->clearFocusedPopup();
                           fp->clearFocusedEmbedded();
                           fp->focusEvent(true);
                           scriptEngine_.notifyFocusedPopupChanged(fp->id(), "");
                       };

                       if (!curFocus.empty()) {
                           // Currently in a popup — next popup, or first embedded, or pane.
                           int cur = -1;
                           for (int i = 0; i < static_cast<int>(popups.size()); ++i) {
                               if (popups[i]->popupId() == curFocus) {
                                   cur = i;
                                   break;
                               }
                           }
                           if (cur >= 0 && cur + 1 < static_cast<int>(popups.size())) {
                               focusPopupIdx(cur + 1);
                           } else if (!embeddedIds.empty()) {
                               focusEmbeddedIdx(0);
                           } else {
                               focusPane();
                           }
                       } else if (curEmbedded != 0) {
                           // Currently in an embedded — next embedded, or pane.
                           size_t cur = embeddedIds.size();
                           for (size_t i = 0; i < embeddedIds.size(); ++i) {
                               if (embeddedIds[i] == curEmbedded) {
                                   cur = i;
                                   break;
                               }
                           }
                           if (cur + 1 < embeddedIds.size()) {
                               focusEmbeddedIdx(cur + 1);
                           } else {
                               focusPane();
                           }
                       } else {
                           // Pane focused — first popup, else first embedded.
                           if (!popups.empty()) {
                               focusPopupIdx(0);
                           } else {
                               focusEmbeddedIdx(0);
                           }
                       }

                       // Mark pane dirty so cursor position updates in the pane texture
                       renderThread_->pending().dirtyPanes.insert(fp->id());
                       setNeedsRedraw();
                   },
                   [&](const Action::ReloadConfig &)
                   {
                       if (configLoader_) {
                           configLoader_->reloadNow();
                       }
                   },
                   [&](const Action::PasteSelection &)
                   {
                       Terminal *term = activeTerm();
                       if (!term || !window_) {
                           return;
                       }
                       const Uuid nodeId      = term->nodeId();
                       auto pasteIfStillAlive = [this, nodeId](std::optional<std::string> text)
                       {
                           if (!text || text->empty()) {
                               return;
                           }
                           if (auto *tt = scriptEngine_.terminal(nodeId)) {
                               tt->pasteText(*text);
                           }
                       };
                       window_->requestSelection(Window::SelectionSource::Primary,
                                                 [this, paste = pasteIfStillAlive](std::optional<std::string> text)
                                                 {
                                                     if (text && !text->empty()) {
                                                         paste(std::move(text));
                                                         return;
                                                     }
                                                     if (window_) {
                                                         window_->requestSelection(Window::SelectionSource::Clipboard, paste);
                                                     }
                                                 });
                   },
                   [&](const Action::ScriptAction &a)
                   {
                       if (!scriptEngine_.isActionRegistered(a.name)) {
                           spdlog::debug("ScriptAction '{}' not registered, ignoring", a.name);
                           return;
                       }
                       // Notification to JS listeners happens via ActionRouter::dispatch
                   },
               },
               action);
}
