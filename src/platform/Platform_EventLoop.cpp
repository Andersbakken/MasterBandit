#include "PlatformDawn.h"
#include "Config.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>


int PlatformDawn::exec()
{
    auto tab = activeTab();
    if (!tab || !tab->valid() || tab->panes().empty()) return 1;

    running_ = true;

    // eventLoop_ and window_ were already created in createTerminal().
    // Here we finish setup: debugIPC, scripts, PTY polls, file watcher, onTick.

    if (hasFlag(FlagHeadless) || hasFlag(FlagIPC)) {
        debugIPC_ = std::make_unique<DebugIPC>(eventLoop_.get(),
            [this]() -> Terminal* { return activeTerm(); },
            [this](int id) {
                auto t = activeTab();
                if (!t || !t->valid()) return std::string{};
                Terminal* pane = t->focusedPane();
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
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    p->injectData(data.c_str(), data.size());
                    return;
                }
            }
        };
        scbs.writePaneToShell = [this](Script::PaneId paneId, const std::string& data) {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    p->writeText(data);
                    return;
                }
            }
        };
        scbs.pastePaneText = [this](Script::PaneId paneId, const std::string& data) {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    p->pasteText(data);
                    return;
                }
            }
        };
        scbs.paneHasPty = [this](Script::PaneId paneId) -> bool {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    return p->masterFD() >= 0;
                }
            }
            return false;
        };
        scbs.hasActiveTab = [this]() -> bool {
            return activeTab().has_value();
        };
        scbs.invokeAction = [this](const std::string& action, const std::vector<std::string>& args) -> bool {
            auto parsed = parseAction(action, args);
            if (!parsed) return false;
            dispatchAction(*parsed);
            return true;
        };
        scbs.paneInfo = [this](Script::PaneId paneId) -> Script::AppCallbacks::PaneInfo {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    bool isFocused = tab.focusedPaneId() == paneId;
                    Script::AppCallbacks::PaneInfo info {
                        p->width(), p->height(),
                        p->title(), p->cwd(),
                        p->masterFD() >= 0,
                        isFocused, p->focusedPopupId(),
                        p->foregroundProcess()
                    };
                    if (!p->nodeId().isNil())
                        info.nodeId = p->nodeId().toString();
                    {
                        const auto& doc = p->document();
                        int absRow = doc.historySize() + p->cursorY();
                        info.cursorLineId = doc.lineIdForAbs(absRow);
                        info.cursorCol    = p->cursorX();
                        info.oldestLineId = doc.lineIdForAbs(0);
                        int total = static_cast<int>(doc.archiveSize()) + doc.historySize() + p->height();
                        info.newestLineId = doc.lineIdForAbs(total - 1);
                        if (p->hasSelection()) {
                            const auto& sel = p->selection();
                            if (sel.valid && !sel.active) {
                                int r0 = sel.startAbsRow, c0 = sel.startCol;
                                int r1 = sel.endAbsRow,   c1 = sel.endCol;
                                if (r0 > r1 || (r0 == r1 && c0 > c1)) {
                                    std::swap(r0, r1); std::swap(c0, c1);
                                }
                                info.hasSelection = true;
                                info.selectionStartLineId = doc.lineIdForAbs(r0);
                                info.selectionStartCol    = c0;
                                info.selectionEndLineId   = doc.lineIdForAbs(r1);
                                info.selectionEndCol      = c1 + 1; // exclusive
                            }
                        }
                        info.selectedCommandId = p->selectedCommandId();
                    }
                    // Mouse position relative to this pane
                    if (inputController_) {
                        PaneRect pr = p->rect();
                        double sx = inputController_->lastCursorX() * contentScaleX_;
                        double sy = inputController_->lastCursorY() * contentScaleY_;
                        double relX = sx - pr.x;
                        double relY = sy - pr.y;
                        if (relX >= 0 && relX < pr.w && relY >= 0 && relY < pr.h) {
                            info.mouseInPane = true;
                            info.mouseCellX  = static_cast<int>(relX / charWidth_);
                            info.mouseCellY  = static_cast<int>(relY / lineHeight_);
                            info.mousePixelX = static_cast<int>(relX);
                            info.mousePixelY = static_cast<int>(relY);
                        }
                    }
                    return info;
                }
            }
            return {};
        };
        scbs.paneCommands = [this](Script::PaneId paneId, int limit) -> std::vector<Script::CommandInfo> {
            std::vector<Script::CommandInfo> result;
            for (Tab tab : tabManager_->tabs()) {
                Terminal* te = tab.valid() ? tab.pane(paneId) : nullptr;
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
                        r.promptStartLineId, r.commandStartLineId,
                        r.outputStartLineId, r.outputEndLineId,
                        doc.firstAbsOfLine(r.promptStartLineId),  r.promptStartCol,
                        doc.firstAbsOfLine(r.commandStartLineId), r.commandStartCol,
                        doc.firstAbsOfLine(r.outputStartLineId),  r.outputStartCol,
                        doc.lastAbsOfLine(r.outputEndLineId),     r.outputEndCol
                    };
                    result.push_back(std::move(info));
                }
                // Callers expect most-recent-last. We collected most-recent-first, so flip.
                std::reverse(result.begin(), result.end());
                break;
            }
            return result;
        };
        scbs.paneSetSelectedCommand = [this](Script::PaneId paneId, std::optional<uint64_t> id) -> bool {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    p->setSelectedCommand(id);
                    return !id.has_value() || p->selectedCommandId() == id;
                }
            }
            return false;
        };
        scbs.paneGetText = [this](Script::PaneId paneId, uint64_t startLineId, int startCol,
                                  uint64_t endLineId, int endCol) -> std::string {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    return p->document().getTextFromLines(startLineId, endLineId, startCol, endCol);
                }
            }
            return {};
        };
        scbs.paneLineIdAt = [this](Script::PaneId paneId, int screenRow) -> std::optional<uint64_t> {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    const auto& doc = p->document();
                    if (screenRow < 0 || screenRow >= doc.rows()) return std::nullopt;
                    int abs = doc.historySize() + screenRow;
                    return doc.lineIdForAbs(abs);
                }
            }
            return std::nullopt;
        };
        scbs.tabs = [this]() -> std::vector<Script::AppCallbacks::TabInfo> {
            std::vector<Script::AppCallbacks::TabInfo> result;
            auto allTabs = tabManager_->tabs();
            int active = tabManager_->activeTabIdx();
            for (int i = 0; i < static_cast<int>(allTabs.size()); ++i) {
                Tab& t = allTabs[i];
                Script::AppCallbacks::TabInfo ti;
                ti.id = i;
                ti.active = (i == active);
                ti.focusedPane = t.valid() ? t.focusedPaneId() : -1;
                if (t.valid()) {
                    for (Terminal* p : t.panes())
                        ti.panes.push_back(p->id());
                }
                Uuid sub = t.subtreeRoot();
                if (!sub.isNil()) ti.nodeId = sub.toString();
                result.push_back(std::move(ti));
            }
            return result;
        };
        scbs.createTab = [this]() -> int {
            createTab();
            return tabManager_->activeTabIdx();
        };
        scbs.closeTab = [this](int tabId) {
            closeTab(tabId);
        };
        scbs.createEmptyTab = [this]() -> std::pair<int, std::string> {
            Uuid nodeId;
            int idx = tabManager_->createEmptyTab(&nodeId);
            return {idx, nodeId.isNil() ? std::string{} : nodeId.toString()};
        };
        scbs.activateTab = [this](int idx) {
            tabManager_->activateTabByIdx(idx);
            tabBarDirty_ = true;
        };
        scbs.focusPane = [this](int paneId) {
            return tabManager_->focusPaneById(paneId);
        };
        scbs.removeNode = [this](Uuid nodeId) {
            return tabManager_->removeNode(nodeId);
        };
        scbs.killTerminalByNodeId = [this](Uuid nodeId) {
            // TabManager::killTerminal mutates live state the render thread
            // observes, so take the platform/render mutex around it (same
            // invariant as closeTab / removeNode).
            std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
            return tabManager_->killTerminal(nodeId);
        };
        scbs.quit = [this]() { quit(); };
        scbs.createTerminalInContainer = [this](const std::string& parentNodeId,
                                                 const std::string& cwd)
                -> Script::AppCallbacks::NewPane {
            Uuid p = Uuid::fromString(parentNodeId);
            if (p.isNil()) return {-1, {}, false};
            int paneId = -1; Uuid n;
            bool ok = tabManager_->createTerminalInContainer(p, cwd, &paneId, &n);
            return {paneId, n.isNil() ? std::string{} : n.toString(), ok};
        };
        scbs.splitPaneByNodeId = [this](const std::string& existingNodeId,
                                         const std::string& dir,
                                         bool newIsFirst)
                -> Script::AppCallbacks::NewPane {
            Uuid p = Uuid::fromString(existingNodeId);
            if (p.isNil()) return {-1, {}, false};
            LayoutNode::Dir d = (dir == "vertical" || dir == "v")
                ? LayoutNode::Dir::Vertical : LayoutNode::Dir::Horizontal;
            int paneId = -1; Uuid n;
            bool ok = tabManager_->splitPaneByNodeId(p, d, /*ratio=*/0.5f,
                                                     newIsFirst, &paneId, &n);
            return {paneId, n.isNil() ? std::string{} : n.toString(), ok};
        };
        scbs.adjustPaneSize = [this](const std::string& paneNodeId,
                                      const std::string& dir, int amount) -> bool {
            Uuid u = Uuid::fromString(paneNodeId);
            if (u.isNil()) return false;
            int paneId = tabManager_->findPaneIdByNodeId(u);
            if (paneId < 0) return false;
            int tabIdx = -1;
            auto tab = tabManager_->findTabForPane(paneId, &tabIdx);
            if (!tab || !tab->valid()) return false;
            LayoutNode::Dir axis;
            int pixelDelta;
            if (dir == "left") {
                axis = LayoutNode::Dir::Horizontal;
                pixelDelta = -static_cast<int>(amount * charWidth_);
            } else if (dir == "right") {
                axis = LayoutNode::Dir::Horizontal;
                pixelDelta = static_cast<int>(amount * charWidth_);
            } else if (dir == "up") {
                axis = LayoutNode::Dir::Vertical;
                pixelDelta = -static_cast<int>(amount * lineHeight_);
            } else if (dir == "down") {
                axis = LayoutNode::Dir::Vertical;
                pixelDelta = static_cast<int>(amount * lineHeight_);
            } else {
                return false;
            }
            if (!tab->resizePaneEdge(paneId, axis, pixelDelta)) return false;
            tabManager_->resizeAllPanesInTab(*tab);
            return true;
        };
        scbs.setZoom = [this](const std::string& paneNodeIdOrEmpty) -> bool {
            auto tab = activeTab();
            if (!tab || !tab->valid()) return false;
            if (paneNodeIdOrEmpty.empty()) {
                tab->unzoom();
                tabManager_->resizeAllPanesInTab(*tab);
                return true;
            }
            Uuid u = Uuid::fromString(paneNodeIdOrEmpty);
            if (u.isNil()) return false;
            int paneId = tabManager_->findPaneIdByNodeId(u);
            if (paneId < 0) return false;
            tab->zoomPane(paneId);
            tabManager_->resizeAllPanesInTab(*tab);
            return true;
        };
        scbs.panePopups = [this](Script::PaneId paneId) -> std::vector<Script::AppCallbacks::PopupInfo> {
            std::vector<Script::AppCallbacks::PopupInfo> result;
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    const std::string& focusedId = p->focusedPopupId();
                    for (const auto& popup : p->popups()) {
                        result.push_back({popup->popupId(), popup->cellX(), popup->cellY(),
                                          popup->cellW(), popup->cellH(),
                                          popup->popupId() == focusedId});
                    }
                    break;
                }
            }
            return result;
        };
        scbs.createPopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h,
                                   std::function<void(const char*, size_t)> onInput) -> bool {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
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
            for (Tab tab : tabManager_->tabs()) {
                Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr;
                if (!p) continue;
                bool wasPopupFocused = (p->focusedPopupId() == popupId);

                // Extract the popup and stage it into the graveyard under
                // the render-thread mutex. The mutex ensures the render
                // thread isn't currently snapshotting, and the stamp taken
                // now will be surpassed once the frame that may already
                // hold a raw pointer to this popup's Terminal completes.
                std::unique_ptr<Terminal> extracted;
                uint64_t stamp = 0;
                {
                    std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                    extracted = p->extractPopup(popupId);
                    if (!extracted) return;
                    // Mirror the removal into the shadow copy so the next
                    // snapshot doesn't hand the render thread a pointer
                    // into the extracted (graveyard-bound) Terminal.
                    for (auto& rpi : renderThread_->renderState().panes) {
                        if (rpi.id != paneId) continue;
                        auto& popups = rpi.popups;
                        popups.erase(std::remove_if(popups.begin(), popups.end(),
                            [&popupId](const RenderPanePopupInfo& pi) { return pi.id == popupId; }),
                            popups.end());
                        if (rpi.focusedPopupId == popupId) rpi.focusedPopupId.clear();
                        break;
                    }
                    stamp = renderThread_->completedFrames();
                }
                graveyard_.defer(std::move(extracted), stamp);

                if (wasPopupFocused && p->popups().empty())
                    p->focusEvent(true);
                p->grid().markAllDirty();
                std::string key = popupStateKey(paneId, popupId);
                renderThread_->pending().structuralOps.push_back(
                    PendingMutations::DestroyPopupState{paneId, popupId});
                renderThread_->pending().releasePopupTextures.push_back(key);
                renderThread_->pending().dirtyPanes.insert(paneId);
                setNeedsRedraw();
                return;
            }
        };
        scbs.resizePopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h) -> bool {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    if (p->resizePopup(popupId, x, y, w, h)) {
                        p->grid().markAllDirty();
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
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* p = tab.valid() ? tab.pane(paneId) : nullptr) {
                    if (Terminal* popup = p->findPopup(popupId)) {
                        popup->injectData(data.c_str(), data.size());
                        setNeedsRedraw();
                    }
                    return;
                }
            }
        };
        scbs.paneUrlAt = [this](Script::PaneId paneId, uint64_t lineId, int col) -> std::string {
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* te = tab.valid() ? tab.pane(paneId) : nullptr) {
                    const auto& doc = te->document();
                    int abs = doc.firstAbsOfLine(lineId);
                    if (abs < 0) return {};
                    int screenRow = abs - doc.historySize();
                    // getExtra works on screen rows; for history rows we need historyExtras
                    const CellExtra* ex = nullptr;
                    if (screenRow >= 0 && screenRow < te->height()) {
                        ex = doc.getExtra(col, screenRow);
                    } else if (abs < doc.historySize()) {
                        auto* extras = doc.historyExtras(abs);
                        if (extras) {
                            auto it = extras->find(col);
                            if (it != extras->end()) ex = &it->second;
                        }
                    }
                    if (ex && ex->hyperlinkId) {
                        const std::string* uri = te->hyperlinkURI(ex->hyperlinkId);
                        if (uri) return *uri;
                    }
                    return {};
                }
            }
            return {};
        };
        scbs.paneGetLinksFromRows = [this](Script::PaneId paneId, uint64_t startLineId, uint64_t endLineId, int limit)
            -> std::vector<Script::AppCallbacks::LinkInfo>
        {
            std::vector<Script::AppCallbacks::LinkInfo> result;
            for (Tab tab : tabManager_->tabs()) {
                if (Terminal* te = tab.valid() ? tab.pane(paneId) : nullptr) {
                    const auto& doc = te->document();
                    int startAbs = doc.firstAbsOfLine(startLineId);
                    int endAbs   = doc.lastAbsOfLine(endLineId);
                    if (startAbs < 0) return result;
                    if (endAbs < 0) endAbs = doc.archiveSize() + doc.historySize() + te->height() - 1;

                    int histSize = doc.historySize();
                    int cols = te->width();
                    uint32_t prevLinkId = 0;

                    for (int abs = startAbs; abs <= endAbs; ++abs) {
                        const std::unordered_map<int, CellExtra>* extras = nullptr;
                        int screenRow = abs - histSize;
                        if (screenRow >= 0 && screenRow < te->height())
                            extras = doc.viewportExtras(screenRow, 0);
                        else if (abs < doc.archiveSize() + histSize)
                            extras = doc.historyExtras(abs);
                        if (!extras) { prevLinkId = 0; continue; }

                        for (int col = 0; col < cols; ++col) {
                            auto it = extras->find(col);
                            uint32_t linkId = (it != extras->end()) ? it->second.hyperlinkId : 0;
                            if (linkId == 0) { prevLinkId = 0; continue; }
                            if (linkId == prevLinkId) {
                                // Extend current link span
                                if (!result.empty()) {
                                    result.back().endLineId = doc.lineIdForAbs(abs);
                                    result.back().endCol = col + 1;
                                }
                                continue;
                            }
                            // New link
                            const std::string* uri = te->hyperlinkURI(linkId);
                            if (!uri) { prevLinkId = 0; continue; }
                            result.push_back({*uri, doc.lineIdForAbs(abs), col, doc.lineIdForAbs(abs), col + 1});
                            prevLinkId = linkId;
                            if (limit > 0 && static_cast<int>(result.size()) >= limit) return result;
                        }
                        prevLinkId = 0; // reset across rows
                    }
                    return result;
                }
            }
            return result;
        };
        scbs.getClipboard = [this](const std::string& source) -> std::string {
            if (!window_) return {};
            if (source == "primary") return window_->getPrimarySelection();
            return window_->getClipboard();
        };
        scbs.setClipboard = [this](const std::string& source, const std::string& text) {
            if (!window_) return;
            if (source == "primary") window_->setPrimarySelection(text);
            else window_->setClipboard(text);
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

    // Load built-in scripts. default-ui.js is mandatory — it owns the
    // JS-policy action handlers (NewTab, SplitPane, ClosePane, etc.) and the
    // terminalExited cascade. A failed load means those actions have no
    // handler, so we refuse to start rather than run in a broken state.
    {
        std::string scriptsDir = exeDir_ + "/scripts/";
        scriptEngine_.setBuiltinModulesDir(scriptsDir + "modules");
        scriptEngine_.loadController(scriptsDir + "applet-loader.js");
        scriptEngine_.loadController(scriptsDir + "command-palette.js");
        if (scriptEngine_.loadController(scriptsDir + "default-ui.js") == 0) {
            spdlog::critical("failed to load default-ui.js from '{}'", scriptsDir);
            std::exit(1);
        }
    }

    // Config file watcher with 300ms debounce + initial apply so that fields
    // not already plumbed through TerminalOptions (OSC 133 dim factor, etc.)
    // pick up user values at startup, not just on hot-reload. Headless/test
    // runs skip the initial apply and the file watch so tests get a clean
    // deterministic environment unaffected by the developer's ~/.config.
    if (configLoader_ && !isHeadless()) {
        ConfigLoader::Host ch;
        ch.eventLoop = eventLoop_.get();
        ch.applyConfig = [this](const Config& c) { applyConfig(c); };
        configLoader_->setHost(std::move(ch));
        configLoader_->installFileWatch(configFilePath());
        configLoader_->reloadNow();
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
                std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                // Advance progress animations
                bool hasAnim = false;
                for (Tab t : tabManager_->tabs()) {
                    if (!t.valid()) continue;
                    for (Terminal* panePtr : t.panes()) {
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

            // Free graveyard entries whose stamp has been surpassed by a
            // completed render frame. Entries were staged from destroy
            // sites under the render-thread mutex, so any frame in flight
            // at stage time has advanced the counter by now; its
            // frameState_ references are out of scope and the held
            // Terminals can safely run their destructors.
            if (renderThread_)
                graveyard_.sweep(renderThread_->completedFrames());

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
