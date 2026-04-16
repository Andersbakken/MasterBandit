#include "PlatformDawn.h"
#include "Config.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>


int PlatformDawn::exec()
{
    Tab* tab = activeTab();
    if (!tab || tab->layout()->panes().empty()) return 1;

    running_ = true;

    // eventLoop_ and window_ were already created in createTerminal().
    // Here we finish setup: debugIPC, scripts, PTY polls, file watcher, onTick.

    if (hasFlag(FlagHeadless) || hasFlag(FlagIPC)) {
        debugIPC_ = std::make_unique<DebugIPC>(eventLoop_.get(),
            [this]() -> Terminal* { return activeTerm(); },
            [this](int id) {
                Tab* t = activeTab();
                if (!t) return std::string{};
                Pane* pane = t->layout()->focusedPane();
                return pane ? gridToJson(pane->id()) : std::string{};
            },
            [this](int id) { return statsJson(id); },
            [this](const std::string& action, const std::vector<std::string>& args) -> bool {
                auto parsed = parseAction(action, args);
                if (!parsed) return false;
                dispatchAction(*parsed);
                return true;
            });
        if (debugSink_) {
            debugSink_->setIPC(debugIPC_.get());
        }
    }

    scriptEngine_.setLoop(eventLoop_.get());

    // Set up script engine callbacks
    {
        Script::AppCallbacks scbs;
        scbs.injectPaneData = [this](Script::PaneId paneId, const std::string& data) {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (auto* t = p->terminal())
                        t->injectData(data.c_str(), data.size());
                    return;
                }
            }
        };
        scbs.injectOverlayData = [this](Script::TabId tabId, const std::string& data) {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay())
                    ov->injectData(data.c_str(), data.size());
            }
        };
        scbs.writePaneToShell = [this](Script::PaneId paneId, const std::string& data) {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (auto* t = p->terminal())
                        t->pasteText(data);
                    return;
                }
            }
        };
        scbs.writeOverlayToShell = [this](Script::TabId tabId, const std::string& data) {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay())
                    ov->pasteText(data);
            }
        };
        scbs.paneHasPty = [this](Script::PaneId paneId) -> bool {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    auto* t = p->terminal();
                    return t && t->masterFD() >= 0;
                }
            }
            return false;
        };
        scbs.overlayHasPty = [this](Script::TabId tabId) -> bool {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay())
                    return ov->masterFD() >= 0;
            }
            return false;
        };
        scbs.hasActiveTab = [this]() -> bool {
            return activeTab() != nullptr;
        };
        scbs.invokeAction = [this](const std::string& action, const std::vector<std::string>& args) -> bool {
            auto parsed = parseAction(action, args);
            if (!parsed) return false;
            dispatchAction(*parsed);
            return true;
        };
        scbs.paneInfo = [this](Script::PaneId paneId) -> Script::AppCallbacks::PaneInfo {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    auto* t = p->terminal();
                    auto* term = dynamic_cast<Terminal*>(t);
                    bool isFocused = tab->layout()->focusedPaneId() == paneId;
                    Script::AppCallbacks::PaneInfo info {
                        t ? t->width() : 0, t ? t->height() : 0,
                        p->title(), p->cwd(),
                        term && term->masterFD() >= 0,
                        isFocused, p->focusedPopupId(),
                        term ? term->foregroundProcess() : std::string{}
                    };
                    if (t && t->hasSelection()) {
                        const auto& sel = t->selection();
                        if (sel.valid && !sel.active) {
                            const auto& doc = t->document();
                            info.hasSelection = true;
                            info.selectionStartRowId = doc.rowIdForAbs(sel.startAbsRow);
                            info.selectionStartCol   = sel.startCol;
                            info.selectionEndRowId   = doc.rowIdForAbs(sel.endAbsRow);
                            info.selectionEndCol     = sel.endCol;
                        }
                    }
                    return info;
                }
            }
            return {};
        };
        scbs.paneCommands = [this](Script::PaneId paneId, int limit) -> std::vector<Script::CommandInfo> {
            std::vector<Script::CommandInfo> result;
            for (auto& tab : tabManager_->tabs()) {
                Pane* p = tab->layout()->pane(paneId);
                if (!p) continue;
                TerminalEmulator* te = p->terminal();
                if (!te) continue;
                const auto& ring = te->commands();
                const auto& doc  = te->document();
                // Walk backwards collecting completed records — this avoids the
                // bug where an in-flight record at the ring's tail would be the
                // "slice window" and then get filtered out, producing an empty
                // list even though earlier completed records exist.
                for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
                    if (!it->complete) continue;
                    if (limit > 0 && result.size() >= static_cast<size_t>(limit)) break;
                    const auto& r = *it;
                    Script::CommandInfo info{
                        r.id, r.cwd, r.exitCode,
                        r.startMs, r.endMs,
                        r.promptStartRowId, r.commandStartRowId,
                        r.outputStartRowId, r.outputEndRowId,
                        doc.absForRowId(r.promptStartRowId),  r.promptStartCol,
                        doc.absForRowId(r.commandStartRowId), r.commandStartCol,
                        doc.absForRowId(r.outputStartRowId),  r.outputStartCol,
                        doc.absForRowId(r.outputEndRowId),    r.outputEndCol
                    };
                    result.push_back(std::move(info));
                }
                // Callers expect most-recent-last. We collected most-recent-first, so flip.
                std::reverse(result.begin(), result.end());
                break;
            }
            return result;
        };
        scbs.paneGetText = [this](Script::PaneId paneId, uint64_t startRowId, int startCol,
                                  uint64_t endRowId, int endCol) -> std::string {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (TerminalEmulator* te = p->terminal())
                        return te->document().getTextFromRows(startRowId, endRowId, startCol, endCol);
                }
            }
            return {};
        };
        scbs.overlayGetText = [this](Script::TabId tabId, uint64_t startRowId, int startCol,
                                     uint64_t endRowId, int endCol) -> std::string {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay())
                    return ov->document().getTextFromRows(startRowId, endRowId, startCol, endCol);
            }
            return {};
        };
        scbs.paneRowIdAt = [this](Script::PaneId paneId, int screenRow) -> std::optional<uint64_t> {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (TerminalEmulator* te = p->terminal()) {
                        const auto& doc = te->document();
                        if (screenRow < 0 || screenRow >= doc.rows()) return std::nullopt;
                        int abs = doc.historySize() + screenRow;
                        return doc.rowIdForAbs(abs);
                    }
                }
            }
            return std::nullopt;
        };
        scbs.overlayRowIdAt = [this](Script::TabId tabId, int screenRow) -> std::optional<uint64_t> {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay()) {
                    const auto& doc = ov->document();
                    if (screenRow < 0 || screenRow >= doc.rows()) return std::nullopt;
                    int abs = doc.historySize() + screenRow;
                    return doc.rowIdForAbs(abs);
                }
            }
            return std::nullopt;
        };
        scbs.overlayInfo = [this](Script::TabId tabId) -> Script::AppCallbacks::OverlayInfo {
            if (Tab* tab = tabManager_->tabAt(tabId)) {
                if (auto* ov = tab->topOverlay())
                    return {ov->width(), ov->height(), ov->masterFD() >= 0, true};
            }
            return {0, 0, false, false};
        };
        scbs.tabs = [this]() -> std::vector<Script::AppCallbacks::TabInfo> {
            std::vector<Script::AppCallbacks::TabInfo> result;
            auto& allTabs = tabManager_->tabs();
            int active = tabManager_->activeTabIdx();
            for (int i = 0; i < static_cast<int>(allTabs.size()); ++i) {
                Script::AppCallbacks::TabInfo ti;
                ti.id = i;
                ti.active = (i == active);
                ti.hasOverlay = allTabs[i]->hasOverlay();
                ti.focusedPane = allTabs[i]->layout()->focusedPaneId();
                for (auto& p : allTabs[i]->layout()->panes())
                    ti.panes.push_back(p->id());
                result.push_back(std::move(ti));
            }
            return result;
        };
        scbs.createOverlay = [this](Script::TabId tabId,
                                     std::function<void(const char*, size_t)> onInput) -> bool {
            Tab* tab = tabManager_->tabAt(tabId);
            if (!tab) return false;
            if (tab->hasOverlay()) return false;

            TerminalCallbacks cbs;
            cbs.event = [this](TerminalEmulator*, int, void*) {
                setNeedsRedraw();
            };

            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [](Terminal*) {};
            pcbs.quit = [this]() { quit(); };
            pcbs.onInput = std::move(onInput);

            auto overlay = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
            TerminalOptions opts = tabManager_->terminalOptions();
            opts.scrollbackLines = 0;
            if (!overlay->initHeadless(opts)) return false;

            float usableW = std::max(0.0f, static_cast<float>(fbWidth_) - padLeft_ - padRight_);
            float usableH = std::max(0.0f, static_cast<float>(fbHeight_) - padTop_ - padBottom_);
            int cols = std::max(1, static_cast<int>(usableW / charWidth_));
            int rows = std::max(1, static_cast<int>(usableH / lineHeight_));
            overlay->resize(cols, rows);

            tab->pushOverlay(std::move(overlay));
            if (tabId == tabManager_->activeTabIdx() && inputController_) inputController_->refreshPointerShape();
            setNeedsRedraw();
            return true;
        };
        scbs.popOverlay = [this](Script::TabId tabId) {
            Tab* tab = tabManager_->tabAt(tabId);
            if (!tab) return;
            if (tab->hasOverlay()) {
                std::lock_guard<std::mutex> plk(renderThread_->mutex());
                tab->popOverlay();
                // Clear stale pointer — render thread may read renderThread_->renderState()
                // before the next buildRenderFrameState() runs.
                renderThread_->renderState().hasOverlay = false;
                renderThread_->renderState().overlay = nullptr;
                if (renderEngine_) renderEngine_->clearOverlayFromFrameState();
                renderThread_->pending().structuralOps.push_back(PendingMutations::DestroyOverlayState{});
                if (tabId == tabManager_->activeTabIdx() && inputController_) inputController_->refreshPointerShape();
                setNeedsRedraw();
            }
        };
        scbs.createTab = [this]() -> int {
            createTab();
            return tabManager_->activeTabIdx();
        };
        scbs.closeTab = [this](int tabId) {
            closeTab(tabId);
        };
        scbs.panePopups = [this](Script::PaneId paneId) -> std::vector<Script::AppCallbacks::PopupInfo> {
            std::vector<Script::AppCallbacks::PopupInfo> result;
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    const std::string& focusedId = p->focusedPopupId();
                    for (const auto& popup : p->popups()) {
                        result.push_back({popup.id, popup.cellX, popup.cellY,
                                          popup.cellW, popup.cellH,
                                          popup.id == focusedId});
                    }
                    break;
                }
            }
            return result;
        };
        scbs.createPopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h,
                                   std::function<void(const char*, size_t)> onInput) -> bool {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    PlatformCallbacks pcbs;
                    pcbs.onTerminalExited = [](Terminal*) {};
                    pcbs.quit = [this]() { quit(); };
                    pcbs.onInput = std::move(onInput);
                    if (p->createPopup(popupId, x, y, w, h, std::move(pcbs))) {
                        // Queue popup render state creation
                        renderThread_->pending().structuralOps.push_back(
                            PendingMutations::CreatePopupState{paneId, popupId, w, h});
                        // Dirty all popup render states for this pane on any content change.
                        p->onPopupEvent = [this, paneId]() {
                            renderThread_->pending().dirtyPanes.insert(paneId);
                            setNeedsRedraw();
                        };
                        // Dirty parent pane so the popup composite entry is added
                        renderThread_->pending().dirtyPanes.insert(paneId);
                        setNeedsRedraw();
                        return true;
                    }
                    return false;
                }
            }
            return false;
        };
        scbs.destroyPopup = [this](Script::PaneId paneId, const std::string& popupId) {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    bool wasPopupFocused = (p->focusedPopupId() == popupId);
                    p->destroyPopup(popupId);
                    Terminal* t = p->terminal();
                    if (wasPopupFocused && t && p->popups().empty())
                        t->focusEvent(true);
                    if (t) t->grid().markAllDirty();
                    // Queue popup render state cleanup
                    std::string key = popupStateKey(paneId, popupId);
                    renderThread_->pending().structuralOps.push_back(
                        PendingMutations::DestroyPopupState{paneId, popupId});
                    renderThread_->pending().releasePopupTextures.push_back(key);
                    // Dirty parent pane
                    renderThread_->pending().dirtyPanes.insert(paneId);
                    setNeedsRedraw();
                    return;
                }
            }
        };
        scbs.resizePopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h) -> bool {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (p->resizePopup(popupId, x, y, w, h)) {
                        if (auto* t = p->terminal()) t->grid().markAllDirty();
                        // Queue popup render state resize
                        renderThread_->pending().structuralOps.push_back(
                            PendingMutations::ResizePopupState{paneId, popupId, w, h});
                        std::string key = popupStateKey(paneId, popupId);
                        renderThread_->pending().releasePopupTextures.push_back(key);
                        setNeedsRedraw();
                        return true;
                    }
                    return false;
                }
            }
            return false;
        };
        scbs.injectPopupData = [this](Script::PaneId paneId, const std::string& popupId,
                                       const std::string& data) {
            for (auto& tab : tabManager_->tabs()) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (PopupPane* popup = p->findPopup(popupId)) {
                        popup->terminal->injectData(data.c_str(), data.size());
                        setNeedsRedraw();
                    }
                    return;
                }
            }
        };
        scriptEngine_.setCallbacks(std::move(scbs));
    }

    // Hook action dispatcher to notify script engine
    if (actionRouter_) {
        actionRouter_->listeners().addListener([this](Action::TypeIndex idx, const Action::Any& action) {
            if (auto* sa = std::get_if<Action::ScriptAction>(&action)) {
                scriptEngine_.notifyAction(sa->name);
            } else {
                scriptEngine_.notifyAction(std::string(Action::nameOf(idx)));
            }
        });
    }

    // Set up script engine config dir
    {
        const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        std::string configDir;
        if (xdgConfig && xdgConfig[0]) {
            configDir = std::string(xdgConfig) + "/MasterBandit";
        } else {
            const char* home = std::getenv("HOME");
            if (home && home[0])
                configDir = std::string(home) + "/.config/MasterBandit";
        }
        if (!configDir.empty())
            scriptEngine_.setConfigDir(configDir);
    }

    // Load built-in scripts
    {
        std::string scriptsDir = exeDir_ + "/scripts/";
        scriptEngine_.setBuiltinModulesDir(scriptsDir + "modules");
        scriptEngine_.loadController(scriptsDir + "applet-loader.js");
        scriptEngine_.loadController(scriptsDir + "command-palette.js");
    }

    // Config file watcher with 300ms debounce
    if (configLoader_) {
        ConfigLoader::Host ch;
        ch.eventLoop = eventLoop_.get();
        ch.applyConfig = [this](const Config& c) { applyConfig(c); };
        configLoader_->setHost(std::move(ch));
        configLoader_->installFileWatch(configFilePath());
    }

    // Set up the per-iteration tick
    if (eventLoop_) {
        eventLoop_->onTick = [this]() {
            if (shouldClose()) {
                eventLoop_->stop();
                return;
            }

            // Structural reads (tabs_, paneRenderStates_, tab-bar state) run
            // under renderThread_->mutex(). The parse itself does NOT — so the render
            // thread can draw frames concurrently during a long flood.
            // Callbacks fired from inside parse that would mutate platform
            // state (notably terminalExited) defer to pendingExits_ and are
            // drained below under the lock.
            {
                std::lock_guard<std::mutex> plk(renderThread_->mutex());
                // Advance progress animations
                bool hasAnim = false;
                for (auto& t : tabManager_->tabs()) {
                    for (auto& panePtr : t->layout()->panes()) {
                        if (panePtr->progressState() == 3) { hasAnim = true; break; }
                    }
                    if (hasAnim) break;
                }
                if (hasAnim) {
                    setNeedsRedraw();
                    auto now = TerminalEmulator::mono();
                    if (tabBarVisible() && now - lastAnimTick_ > 100) {
                        lastAnimTick_ = now;
                        tabBarAnimFrame_++;
                        tabBarDirty_ = true;
                    }
                }
            }

            // Flush coalesced PTY reads — no renderThread_->mutex(), so the render
            // thread can run concurrently. Terminal mutation is serialized by
            // TerminalEmulator::mutex() inside injectData. Structural
            // callbacks (terminalExited) defer their work; we drain them
            // under renderThread_->mutex() right after.
            for (auto& [fd, term] : tabManager_->ptyPolls())
                term->flushReadBuffer();

            // Drain deferred callbacks.
            drainDeferredMain();         // title / icon / cwd / progress / etc.
            scriptEngine_.executePendingJobs();

            // Apply all accumulated mutations to the shadow render state in
            // one batch under renderThread_->mutex().  drainPendingExits() runs
            // inside the lock so that terminal destruction can't race the
            // render thread's use of frameState_ term pointers.
            applyPendingMutations();

            // Flush any pending TIOCSWINSZ on the main thread so the render
            // thread never mutates terminal state.
            if (!window_ || !window_->inLiveResize()) {
                for (auto& [fd, term] : tabManager_->ptyPolls())
                    term->flushPendingResize();
            }

            // Render thread may have requested an animation wakeup — wire
            // the event-loop timer on the main thread.
            if (animScheduler_) animScheduler_->applyPendingAnimation();



            // device_.Tick() is called from the render thread (see
            // renderThreadMain). Calling it here while the render thread
            // is mid-encode triggers a Metal assertion:
            //   "encodeSignalEvent:value: with uncommitted encoder".

            bool redraw = renderEngine_ && renderEngine_->needsRedraw();
            int pendingCbs = renderEngine_ ? renderEngine_->pendingGpuCallbacks() : 0;
            if (redraw ||
                (debugIPC_ && debugIPC_->pngScreenshotPending()) ||
                pendingCbs > 0) {
                wakeRenderThread();
            }

            // If something during this tick requested another frame (e.g. animation),
            // ensure the event loop doesn't sleep.
            if (redraw || pendingCbs > 0)
                eventLoop_->wakeup();
        };
    }

    // Start cursor blink timer from current options (default 500ms).
    if (animScheduler_) {
        const auto& cur = tabManager_->terminalOptions().cursor;
        animScheduler_->applyBlinkConfig(cur.blink_rate, cur.blink_fps);
    }

    if (eventLoop_) eventLoop_->run();

    if (debugSink_) debugSink_->setIPC(nullptr);

    // Cleanup
    std::vector<int> fds;
    for (auto& [fd, _] : tabManager_->ptyPolls()) fds.push_back(fd);
    for (int fd : fds) tabManager_->removePtyPoll(fd);

    if (configLoader_) configLoader_->stop();
    if (animScheduler_) animScheduler_->stopAllTimers();
    eventLoop_->removeFileWatch();

    if (debugIPC_) debugIPC_->closeHandles();
    debugIPC_.reset();

    return exitStatus_;
}


void PlatformDawn::quit(int status)
{
    exitStatus_ = status;
    if (eventLoop_) {
        eventLoop_->stop();
    }
}


void PlatformDawn::setNeedsRedraw()
{
    if (renderEngine_) renderEngine_->setNeedsRedraw();
}
