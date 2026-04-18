#include "PlatformDawn.h"
#include "PlatformUtils.h"
#include "Utils.h"

// Forwarders. The data and behavior live in TabManager; PlatformDawn
// keeps the entry points so existing call sites (Platform_Actions.cpp,
// Platform_EventLoop.cpp's ScriptEngine callbacks) don't need to
// reach through tabManager_ explicitly.
void PlatformDawn::createTab() { if (tabManager_) tabManager_->createTab(); }
void PlatformDawn::closeTab(int idx) { if (tabManager_) tabManager_->closeTab(idx); }


TerminalCallbacks PlatformDawn::buildTerminalCallbacks(int paneId)
{
    TerminalCallbacks cbs;

    cbs.event = [this, paneId](TerminalEmulator*, int ev, void* payload) {
        switch (static_cast<TerminalEmulator::Event>(ev)) {
        case TerminalEmulator::Update:
        case TerminalEmulator::ScrollbackChanged:
            // setNeedsRedraw uses atomic; safe from any thread. Mutation of
            // paneRenderStates_.dirty is deferred to main thread.
            setNeedsRedraw();
            postToMainThread([this, paneId] {
                renderThread_->pending().dirtyPanes.insert(paneId);
            });
            break;
        case TerminalEmulator::VisibleBell:
            break;
        case TerminalEmulator::CommandComplete:
            if (payload) {
                // Copy the record out of the callback payload — it may not
                // outlive the deferred lambda.
                const auto* rec = static_cast<const TerminalEmulator::CommandRecord*>(payload);
                TerminalEmulator::CommandRecord recCopy = *rec;
                postToMainThread([this, paneId, recCopy = std::move(recCopy)] {
                    TerminalEmulator* te = nullptr;
                    for (auto& tab : tabManager_->tabs()) {
                        if (auto* p = tab->layout()->pane(paneId)) { te = p->terminal(); break; }
                    }
                    if (!te) return;
                    const auto& doc = te->document();
                    Script::CommandInfo info{
                        recCopy.id, recCopy.cwd, recCopy.exitCode,
                        recCopy.startMs, recCopy.endMs,
                        recCopy.promptStartLineId, recCopy.commandStartLineId,
                        recCopy.outputStartLineId, recCopy.outputEndLineId,
                        doc.firstAbsOfLine(recCopy.promptStartLineId),  recCopy.promptStartCol,
                        doc.firstAbsOfLine(recCopy.commandStartLineId), recCopy.commandStartCol,
                        doc.firstAbsOfLine(recCopy.outputStartLineId),  recCopy.outputStartCol,
                        doc.lastAbsOfLine(recCopy.outputEndLineId),     recCopy.outputEndCol
                    };
                    scriptEngine_.notifyCommandComplete(paneId, info);
                });
            }
            break;
        case TerminalEmulator::CommandSelectionChanged:
            // Dispatch to the script engine on the main thread — the live
            // selection id is read fresh inside the lambda so late-arriving
            // events see the final state even if multiple fire in a row.
            postToMainThread([this, paneId] {
                TerminalEmulator* te = nullptr;
                for (auto& tab : tabManager_->tabs()) {
                    if (auto* p = tab->layout()->pane(paneId)) { te = p->terminal(); break; }
                }
                if (!te) return;
                scriptEngine_.notifyCommandSelectionChanged(paneId, te->selectedCommandId());
            });
            break;
        }
    };

    if (!isHeadless()) {
        cbs.copyToClipboard = [this](const std::string& text) {
            // NSPasteboard access must stay on the main thread.
            postToMainThread([this, text] {
                if (window_) window_->setClipboard(text);
            });
        };
        cbs.pasteFromClipboard = [this]() -> std::string {
            // Synchronous read. Called while parse holds Terminal mutex on
            // the main thread today; if Phase 5+ moves it, re-evaluate.
            return window_ ? window_->getClipboard() : std::string{};
        };
    } else {
        cbs.copyToClipboard = [](const std::string&) {};
        cbs.pasteFromClipboard = []() -> std::string { return {}; };
    }

    cbs.onTitleChanged = [this, paneId](const std::string& title) {
        postToMainThread([this, paneId, title] {
            int tabIdx = -1;
            Tab* t = tabManager_->findTabForPane(paneId, &tabIdx);
            if (!t) return;
            if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
            if (t->layout()->focusedPaneId() == paneId) {
                t->setTitle(title);
                if (tabIdx == tabManager_->activeTabIdx()) tabManager_->updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onIconChanged = [this, paneId](const std::string& icon) {
        postToMainThread([this, paneId, icon] {
            int tabIdx = -1;
            Tab* t = tabManager_->findTabForPane(paneId, &tabIdx);
            if (!t) return;
            if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
            if (t->layout()->focusedPaneId() == paneId) {
                t->setIcon(icon);
                if (tabIdx == tabManager_->activeTabIdx()) tabManager_->updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        postToMainThread([this, paneId, state, pct] {
            Tab* t = tabManager_->findTabForPane(paneId);
            if (!t) return;
            if (Pane* p = t->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = isHeadless() ? []() { return true; } : []() { return platformIsDarkMode(); };

    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        postToMainThread([this, paneId, dir] {
            Tab* t = tabManager_->findTabForPane(paneId);
            if (!t) return;
            if (Pane* p = t->layout()->pane(paneId)) p->setCWD(dir);
        });
    };

    if (isHeadless()) {
        cbs.onDesktopNotification = [](const std::string&, const std::string&, const std::string&) {};
    } else {
        cbs.onDesktopNotification = [this](const std::string& title, const std::string& body, const std::string& /*icon*/) {
            // Route through main thread — platformSendNotification calls into
            // NSUserNotificationCenter / D-Bus which are main-thread affine.
            postToMainThread([title, body] {
                platformSendNotification(title, body);
            });
        };
    }

    cbs.onOSC = [this, paneId](int oscNum, std::string_view payload) {
        // Script dispatch — scripts may mutate anything. Defer so the script
        // runs on the main thread under renderThread_->mutex().
        std::string payloadCopy(payload);
        postToMainThread([this, paneId, oscNum, payloadCopy = std::move(payloadCopy)] {
            scriptEngine_.notifyOSC(paneId, oscNum, payloadCopy);
        });
    };

    cbs.customTcapLookup = [this](const std::string& name) -> std::optional<std::string> {
        return scriptEngine_.lookupCustomTcap(name);
    };

    cbs.onMouseCursorShape = [this, paneId](const std::string& shape) {
        postToMainThread([this, paneId, shape] {
            // Cache the converted style per pane; cursor-pos updates read this
            // without re-parsing the CSS name on every mouse move.
            if (!inputController_) return;
            if (shape.empty()) {
                inputController_->erasePaneCursorStyle(paneId);
            } else {
                inputController_->setPaneCursorStyle(paneId,
                    InputController::pointerShapeNameToCursorStyle(shape));
            }
            // Apply immediately if the request is from the focused pane in the
            // active tab; otherwise the next mouse-move into that pane will pick
            // it up. Background panes/tabs don't fight over the cursor.
            if (!window_ || isHeadless()) return;
            Tab* t = tabManager_->activeTab();
            if (!t || t->layout()->focusedPaneId() != paneId) return;
            window_->setCursorStyle(shape.empty()
                ? Window::CursorStyle::IBeam
                : inputController_->paneCursorStyle(paneId));
        });
    };

    cbs.onForegroundProcessChanged = [this, paneId](const std::string& proc) {
        postToMainThread([this, paneId, proc] {
            scriptEngine_.notifyForegroundProcessChanged(paneId, proc);
            // Use foreground process as tab title if pane has no OSC title
            int tabIdx = -1;
            Tab* t = tabManager_->findTabForPane(paneId, &tabIdx);
            if (!t) return;
            Pane* p = t->layout()->pane(paneId);
            if (p && p->title().empty() && t->layout()->focusedPaneId() == paneId) {
                t->setTitle(proc);
                if (tabIdx == tabManager_->activeTabIdx()) tabManager_->updateWindowTitle();
                tabBarDirty_ = true;
                setNeedsRedraw();
            }
        });
    };

    return cbs;
}
