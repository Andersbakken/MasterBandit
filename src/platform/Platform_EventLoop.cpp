#include "PlatformDawn.h"
#include "Config.h"
#include "Resources.h"
#include <glaze/glaze.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>


int PlatformDawn::exec()
{
    // No tab-must-exist guard here: the first tab and its initial Terminal
    // are built by default-ui.js during script loading below. Scripts run
    // synchronously before the event loop starts, so by the time we enter
    // the loop the tab is there.
    running_ = true;

    // eventLoop_ and window_ were already created in createTerminal().
    // Here we finish setup: debugIPC, scripts, PTY polls, file watcher, onTick.

    if (hasFlag(FlagHeadless) || hasFlag(FlagIPC)) {
        debugIPC_ = std::make_unique<DebugIPC>(eventLoop_.get(),
            [this]() -> Terminal* { return activeTerm(); },
            [this](int id) {
                auto t = activeTab();
                if (!t) return std::string{};
                Terminal* pane = scriptEngine_.focusedTerminalInSubtree(*t);
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
        scbs.requestRedraw = [this]() { setNeedsRedraw(); };
        scbs.fontCellSize = [this]() -> std::pair<float, float> {
            return { charWidth_, lineHeight_ };
        };
        scbs.configJson = [this]() -> std::string {
            std::string buf;
            (void)glz::write_json(lastConfig_, buf);
            return buf;
        };
        scbs.writePaneToShell = [this](Script::PaneId paneId, const std::string& data) {
            if (Terminal* p = scriptEngine_.terminal(paneId)) p->writeText(data);
        };
        scbs.pastePaneText = [this](Script::PaneId paneId, const std::string& data) {
            if (Terminal* p = scriptEngine_.terminal(paneId)) p->pasteText(data);
        };
        scbs.paneHasPty = [this](Script::PaneId paneId) -> bool {
            if (Terminal* p = scriptEngine_.terminal(paneId)) return p->masterFD() >= 0;
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
            if (Terminal* p = scriptEngine_.terminal(paneId)) {
                    bool isFocused = scriptEngine_.focusedTerminalNodeId() == paneId;
                    // PaneInfo.title is a plain string for JS; collapse
                    // the optional here (nullopt → "").
                    Script::AppCallbacks::PaneInfo info {
                        p->width(), p->height(),
                        p->title().value_or(std::string{}), p->cwd(),
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
                        if (auto resOpt = p->resolveSelection(); resOpt && resOpt->valid && !resOpt->active) {
                            const auto& res = *resOpt;
                            int r0 = res.startAbsRow, c0 = res.startCol;
                            int r1 = res.endAbsRow,   c1 = res.endCol;
                            if (r0 > r1 || (r0 == r1 && c0 > c1)) {
                                std::swap(r0, r1); std::swap(c0, c1);
                            }
                            info.hasSelection = true;
                            info.selectionStartLineId = doc.lineIdForAbs(r0);
                            info.selectionStartCol    = c0;
                            info.selectionEndLineId   = doc.lineIdForAbs(r1);
                            info.selectionEndCol      = c1 + 1; // exclusive
                        }
                        info.selectedCommandId = p->selectedCommandId();
                    }
                    // Mouse position relative to this pane
                    if (inputController_) {
                        Rect pr = p->rect();
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
            return {};
        };
        scbs.paneCommands = [this](Script::PaneId paneId, int limit) -> std::vector<Script::CommandInfo> {
            std::vector<Script::CommandInfo> result;
            Terminal* te = scriptEngine_.terminal(paneId);
            if (!te) return result;
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
            return result;
        };
        scbs.paneSetSelectedCommand = [this](Script::PaneId paneId, std::optional<uint64_t> id) -> bool {
            if (Terminal* p = scriptEngine_.terminal(paneId)) {
                p->setSelectedCommand(id);
                return !id.has_value() || p->selectedCommandId() == id;
            }
            return false;
        };
        scbs.paneGetText = [this](Script::PaneId paneId, uint64_t startLineId, int startCol,
                                  uint64_t endLineId, int endCol) -> std::string {
            if (Terminal* p = scriptEngine_.terminal(paneId))
                return p->document().getTextFromLines(startLineId, endLineId, startCol, endCol);
            return {};
        };
        scbs.paneLineIdAt = [this](Script::PaneId paneId, int screenRow) -> std::optional<uint64_t> {
            if (Terminal* p = scriptEngine_.terminal(paneId)) {
                const auto& doc = p->document();
                if (screenRow < 0 || screenRow >= doc.rows()) return std::nullopt;
                int abs = doc.historySize() + screenRow;
                return doc.lineIdForAbs(abs);
            }
            return std::nullopt;
        };
        scbs.tabs = [this]() -> std::vector<Script::AppCallbacks::TabInfo> {
            std::vector<Script::AppCallbacks::TabInfo> result;
            Uuid activeSub = scriptEngine_.activeTabSubtreeRoot();
            for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
                Script::AppCallbacks::TabInfo ti;
                ti.id = sub;
                ti.active = (sub == activeSub);
                ti.focusedPane = scriptEngine_.focusedPaneInSubtree(sub);
                for (Terminal* p : scriptEngine_.panesInSubtree(sub))
                    ti.panes.push_back(p->nodeId());
                if (!sub.isNil()) ti.nodeId = sub.toString();
                result.push_back(std::move(ti));
            }
            return result;
        };
        scbs.closeTab = [this](Uuid sub) {
            closeTab(sub);
        };
        scbs.createEmptyTab = [this]() -> Uuid {
            return createEmptyTab();
        };
        scbs.activateTab = [this](Uuid sub) {
            activateTabByUuid(sub);
            tabBarDirty_ = true;
        };
        scbs.focusPane = [this](Uuid nodeId) {
            return focusPaneById(nodeId);
        };
        scbs.removeNode = [this](Uuid nodeId) {
            return removeNode(nodeId);
        };
        scbs.killTerminalByNodeId = [this](Uuid nodeId) {
            std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
            return killTerminal(nodeId);
        };
        scbs.quit = [this]() { quit(); };
        scbs.createTerminalInContainer = [this](const std::string& parentNodeId,
                                                 const std::string& cwd)
                -> Script::AppCallbacks::NewPane {
            Uuid p = Uuid::fromString(parentNodeId);
            if (p.isNil()) return {{}, false};
            Uuid n;
            bool ok = createTerminalInContainer(p, cwd, &n);
            return {n.isNil() ? std::string{} : n.toString(), ok};
        };
        scbs.splitPaneByNodeId = [this](const std::string& existingNodeId,
                                         const std::string& dir,
                                         bool newIsFirst)
                -> Script::AppCallbacks::NewPane {
            Uuid p = Uuid::fromString(existingNodeId);
            if (p.isNil()) return {{}, false};
            SplitDir d = (dir == "vertical" || dir == "v")
                ? SplitDir::Vertical : SplitDir::Horizontal;
            Uuid n;
            bool ok = splitPaneByNodeId(p, d, /*ratio=*/0.5f,
                                                     newIsFirst, &n);
            return {n.isNil() ? std::string{} : n.toString(), ok};
        };
        scbs.adjustPaneSize = [this](const std::string& paneNodeId,
                                      const std::string& dir, int amount) -> bool {
            Uuid u = Uuid::fromString(paneNodeId);
            if (u.isNil()) return false;
            auto tab = findTabForPane(u);
            if (!tab) return false;
            SplitDir axis;
            int pixelDelta;
            if (dir == "left") {
                axis = SplitDir::Horizontal;
                pixelDelta = -static_cast<int>(amount * charWidth_);
            } else if (dir == "right") {
                axis = SplitDir::Horizontal;
                pixelDelta = static_cast<int>(amount * charWidth_);
            } else if (dir == "up") {
                axis = SplitDir::Vertical;
                pixelDelta = -static_cast<int>(amount * lineHeight_);
            } else if (dir == "down") {
                axis = SplitDir::Vertical;
                pixelDelta = static_cast<int>(amount * lineHeight_);
            } else {
                return false;
            }
            if (!scriptEngine_.resizeTabPaneEdge(*tab, u, axis, pixelDelta)) return false;
            resizeAllPanesInTab(*tab);
            return true;
        };
        scbs.setStackZoom = [this](const std::string& stackNodeIdStr,
                                    const std::string& targetNodeIdOrEmpty) -> bool {
            Uuid stackId = Uuid::fromString(stackNodeIdStr);
            if (stackId.isNil()) return false;
            Uuid targetId;
            if (!targetNodeIdOrEmpty.empty()) {
                targetId = Uuid::fromString(targetNodeIdOrEmpty);
                if (targetId.isNil()) return false;
            }
            LayoutTree& tree = scriptEngine_.layoutTree();
            if (!tree.setStackZoom(stackId, targetId)) return false;
            // Trigger resize cascade on the enclosing tab so terminals pick
            // up the new rects (shrunk siblings, zoom target expanded).
            if (auto tab = findTabForNode(stackId)) resizeAllPanesInTab(*tab);
            return true;
        };
        scbs.panePopups = [this](Script::PaneId paneId) -> std::vector<Script::AppCallbacks::PopupInfo> {
            std::vector<Script::AppCallbacks::PopupInfo> result;
            if (Terminal* p = scriptEngine_.terminal(paneId)) {
                const std::string& focusedId = p->focusedPopupId();
                for (const auto& popup : p->popups()) {
                    result.push_back({popup->popupId(), popup->cellX(), popup->cellY(),
                                      popup->cellW(), popup->cellH(),
                                      popup->popupId() == focusedId});
                }
            }
            return result;
        };
        scbs.createPopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h,
                                   std::function<void(const char*, size_t)> onInput) -> bool {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p) return false;
            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [](Terminal*) {};
            pcbs.quit = [this]() { quit(); };
            pcbs.onInput = std::move(onInput);
            if (!p->createPopup(popupId, x, y, w, h, std::move(pcbs))) return false;
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
        };
        scbs.destroyPopup = [this](Script::PaneId paneId, const std::string& popupId) {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p) return;
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
        };
        scbs.resizePopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h) -> bool {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p || !p->resizePopup(popupId, x, y, w, h)) return false;
            p->grid().markAllDirty();
            renderThread_->pending().structuralOps.push_back(
                PendingMutations::ResizePopupState{paneId, popupId, w, h});
            std::string key = popupStateKey(paneId, popupId);
            renderThread_->pending().releasePopupTextures.push_back(key);
            scriptEngine_.deliverPopupResized(paneId, popupId, w, h);
            setNeedsRedraw();
            return true;
        };
        scbs.paneEmbeddeds = [this](Script::PaneId paneId) -> std::vector<Script::AppCallbacks::EmbeddedInfo> {
            std::vector<Script::AppCallbacks::EmbeddedInfo> result;
            if (Terminal* p = scriptEngine_.terminal(paneId)) {
                const uint64_t focused = p->focusedEmbeddedLineId();
                for (const auto& [lineId, em] : p->embeddeds()) {
                    result.push_back({lineId, em->height(), lineId == focused});
                }
            }
            return result;
        };
        scbs.createEmbedded = [this](Script::PaneId paneId, int rows) -> uint64_t {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p) return 0;
            // Resolve anchor lineId from current cursor position BEFORE the
            // call, because createEmbedded advances the cursor internally.
            // The document's lineIdForAbs on the cursor's absolute row is the
            // captured anchor — we need it now so the onInput delivery lambda
            // can bake the regKey in at install time.
            uint64_t anchorLineId = p->document().lineIdForAbs(
                p->document().historySize() + p->cursorY());
            if (anchorLineId == 0) return 0;

            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [](Terminal*) {};
            pcbs.quit = [this]() { quit(); };
            // When the user types into a focused embedded, its headless
            // Terminal emits bytes via writeToOutput → onInput. Route to the
            // JS "input" listeners registered under "paneId:lineId" — same
            // regKey-based fanout as deliverEmbeddedDestroyed.
            std::string regKey = paneId.toString() + ":" + std::to_string(anchorLineId);
            pcbs.onInput = [this, regKey](const char* data, size_t len) {
                scriptEngine_.deliverEmbeddedInput(regKey, data, len);
                scriptEngine_.executePendingJobs();
            };

            Terminal* em = p->createEmbedded(rows, std::move(pcbs));
            if (!em) return 0;
            setNeedsRedraw();
            return anchorLineId;
        };
        scbs.destroyEmbedded = [this](Script::PaneId paneId, uint64_t lineId) {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p) return;

            // Mirror the popup-destruction pattern: under the render mutex,
            // extract the embedded from the live parent, remove it from the
            // shadow copy, and stamp with the current completed-frames
            // counter. Release the mutex before handing the unique_ptr to
            // the graveyard — any frame that holds a raw pointer to this
            // embedded Terminal has until it completes to finish; the
            // graveyard will sweep it later once completedFrames > stamp.
            std::unique_ptr<Terminal> extracted;
            uint64_t stamp = 0;
            {
                std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                extracted = p->extractEmbedded(lineId);
                if (!extracted) return;
                for (auto& rpi : renderThread_->renderState().panes) {
                    if (rpi.id != paneId) continue;
                    auto& embs = rpi.embeddeds;
                    embs.erase(std::remove_if(embs.begin(), embs.end(),
                        [lineId](const RenderPaneEmbeddedInfo& ei) { return ei.lineId == lineId; }),
                        embs.end());
                    if (rpi.focusedEmbeddedLineId == lineId) rpi.focusedEmbeddedLineId = 0;
                    break;
                }
                stamp = renderThread_->completedFrames();
            }
            graveyard_.defer(std::move(extracted), stamp);

            renderThread_->pending().structuralOps.push_back(
                PendingMutations::DestroyEmbeddedState{paneId, lineId});
            renderThread_->pending().releaseEmbeddedTextures.push_back(
                paneId.toString() + ":" + std::to_string(lineId));
            renderThread_->pending().dirtyPanes.insert(paneId);
            setNeedsRedraw();
        };
        scbs.resizeEmbedded = [this](Script::PaneId paneId, uint64_t lineId, int rows) -> bool {
            Terminal* p = scriptEngine_.terminal(paneId);
            if (!p) return false;
            bool ok = p->resizeEmbedded(lineId, rows);
            if (ok) {
                // Parent-cascade resize (pane cols change) isn't wired yet;
                // if/when it lands, fire the event from there too so applets
                // see all size changes uniformly.
                scriptEngine_.deliverEmbeddedResized(paneId, lineId, p->width(), rows);
                setNeedsRedraw();
            }
            return ok;
        };
        scbs.paneUrlAt = [this](Script::PaneId paneId, uint64_t lineId, int col) -> std::string {
            Terminal* te = scriptEngine_.terminal(paneId);
            if (!te) return {};
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
        };
        scbs.paneGetLinksFromRows = [this](Script::PaneId paneId, uint64_t startLineId, uint64_t endLineId, int limit)
            -> std::vector<Script::AppCallbacks::LinkInfo>
        {
            std::vector<Script::AppCallbacks::LinkInfo> result;
            Terminal* te = scriptEngine_.terminal(paneId);
            if (te) {
                {
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
        std::string scriptsDir = Resources::path("scripts").string() + "/";
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
        configLoader_->setPlatform(this);
        configLoader_->installFileWatch(configFilePath());
        configLoader_->reloadNow();
    }

    // Set up the per-iteration tick
    if (eventLoop_) {
        eventLoop_->onQuitRequested = [this]() { quit(); };
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
                for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
                    for (Terminal* panePtr : scriptEngine_.panesInSubtree(sub)) {
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
            for (auto& [fd, term] : ptyPolls())
                term->flushReadBuffer();

            // Drain embeddeds whose anchor line evicted during the PTY flush
            // above. The eviction callback stashed them on the parent
            // Terminal because it couldn't take the render mutex from inside
            // Document::evictToArchive. Now that the terminal mutex is
            // released, we can go through the same render-mutex + graveyard
            // path used by destroyEmbedded.
            for (Uuid sub : scriptEngine_.tabSubtreeRoots()) {
                for (Terminal* pane : scriptEngine_.panesInSubtree(sub)) {
                    if (!pane->hasEvictedEmbeddeds()) continue;
                    Uuid paneId = pane->nodeId();
                    pane->drainEvictedEmbeddeds(
                        [&, this, paneId](uint64_t lineId, std::unique_ptr<Terminal> em) {
                            uint64_t stamp = 0;
                            {
                                std::lock_guard<std::recursive_mutex> plk(renderThread_->mutex());
                                for (auto& rpi : renderThread_->renderState().panes) {
                                    if (rpi.id != paneId) continue;
                                    auto& embs = rpi.embeddeds;
                                    embs.erase(std::remove_if(embs.begin(), embs.end(),
                                        [lineId](const RenderPaneEmbeddedInfo& ei) { return ei.lineId == lineId; }),
                                        embs.end());
                                    if (rpi.focusedEmbeddedLineId == lineId) rpi.focusedEmbeddedLineId = 0;
                                    break;
                                }
                                stamp = renderThread_->completedFrames();
                            }
                            graveyard_.defer(std::move(em), stamp);
                            renderThread_->pending().structuralOps.push_back(
                                PendingMutations::DestroyEmbeddedState{paneId, lineId});
                            renderThread_->pending().releaseEmbeddedTextures.push_back(
                                paneId.toString() + ":" + std::to_string(lineId));
                            renderThread_->pending().dirtyPanes.insert(paneId);

                            // Fire JS "destroyed" listeners for this embedded.
                            scriptEngine_.deliverEmbeddedDestroyed(
                                paneId.toString() + ":" + std::to_string(lineId));
                        });
                    setNeedsRedraw();
                }
            }

            // Drain deferred callbacks.
            renderThread_->drainDeferredMain();         // title / icon / cwd / progress / etc.
            scriptEngine_.executePendingJobs();

            // Apply all accumulated mutations to the shadow render state in
            // one batch under renderThread_->mutex().  renderThread_->drainPendingExits() runs
            // inside the lock so that terminal destruction can't race the
            // render thread's use of frameState_ term pointers.
            renderThread_->applyPendingMutations();

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
                for (auto& [fd, term] : ptyPolls())
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
                renderThread_->wake();
            }

            // If something during this tick requested another frame (e.g. animation),
            // ensure the event loop doesn't sleep.
            if (redraw || pendingCbs > 0)
                eventLoop_->wakeup();
        };
    }

    // Start cursor blink timer from current options (default 500ms).
    if (animScheduler_) {
        const auto& cur = terminalOptions().cursor;
        animScheduler_->applyBlinkConfig(cur.blink_rate, cur.blink_fps);
    }

    if (eventLoop_) eventLoop_->run();

    if (debugSink_) debugSink_->setIPC(nullptr);

    // Cleanup
    std::vector<int> fds;
    for (auto& [fd, _] : ptyPolls()) fds.push_back(fd);
    for (int fd : fds) removePtyPoll(fd);

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
