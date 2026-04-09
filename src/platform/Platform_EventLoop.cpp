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

    if (headless_) {
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
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (auto* t = p->terminal())
                        t->injectData(data.c_str(), data.size());
                    return;
                }
            }
        };
        scbs.injectOverlayData = [this](Script::TabId tabId, const std::string& data) {
            if (tabId >= 0 && tabId < static_cast<int>(tabs_.size())) {
                if (auto* ov = tabs_[tabId]->topOverlay())
                    ov->injectData(data.c_str(), data.size());
            }
        };
        scbs.writePaneToShell = [this](Script::PaneId paneId, const std::string& data) {
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (auto* t = p->terminal())
                        t->pasteText(data);
                    return;
                }
            }
        };
        scbs.writeOverlayToShell = [this](Script::TabId tabId, const std::string& data) {
            if (tabId >= 0 && tabId < static_cast<int>(tabs_.size())) {
                if (auto* ov = tabs_[tabId]->topOverlay())
                    ov->pasteText(data);
            }
        };
        scbs.paneHasPty = [this](Script::PaneId paneId) -> bool {
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    auto* t = p->terminal();
                    return t && t->masterFD() >= 0;
                }
            }
            return false;
        };
        scbs.overlayHasPty = [this](Script::TabId tabId) -> bool {
            if (tabId >= 0 && tabId < static_cast<int>(tabs_.size())) {
                if (auto* ov = tabs_[tabId]->topOverlay())
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
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    auto* t = p->terminal();
                    auto* term = dynamic_cast<Terminal*>(t);
                    bool isFocused = tab->layout()->focusedPaneId() == paneId;
                    return {
                        t ? t->width() : 0, t ? t->height() : 0,
                        p->title(), p->cwd(),
                        term && term->masterFD() >= 0,
                        isFocused, p->focusedPopupId(),
                        term ? term->foregroundProcess() : std::string{}
                    };
                }
            }
            return {};
        };
        scbs.overlayInfo = [this](Script::TabId tabId) -> Script::AppCallbacks::OverlayInfo {
            if (tabId >= 0 && tabId < static_cast<int>(tabs_.size())) {
                if (auto* ov = tabs_[tabId]->topOverlay())
                    return {ov->width(), ov->height(), ov->masterFD() >= 0, true};
            }
            return {0, 0, false, false};
        };
        scbs.tabs = [this]() -> std::vector<Script::AppCallbacks::TabInfo> {
            std::vector<Script::AppCallbacks::TabInfo> result;
            for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
                Script::AppCallbacks::TabInfo ti;
                ti.id = i;
                ti.active = (i == activeTabIdx_);
                ti.hasOverlay = tabs_[i]->hasOverlay();
                ti.focusedPane = tabs_[i]->layout()->focusedPaneId();
                for (auto& p : tabs_[i]->layout()->panes())
                    ti.panes.push_back(p->id());
                result.push_back(std::move(ti));
            }
            return result;
        };
        scbs.createOverlay = [this](Script::TabId tabId,
                                     std::function<void(const char*, size_t)> onInput) -> bool {
            if (tabId < 0 || tabId >= static_cast<int>(tabs_.size())) return false;
            Tab* tab = tabs_[tabId].get();
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
            TerminalOptions opts = terminalOptions_;
            opts.scrollbackLines = 0;
            if (!overlay->initHeadless(opts)) return false;

            float usableW = std::max(0.0f, static_cast<float>(fbWidth_) - padLeft_ - padRight_);
            float usableH = std::max(0.0f, static_cast<float>(fbHeight_) - padTop_ - padBottom_);
            int cols = std::max(1, static_cast<int>(usableW / charWidth_));
            int rows = std::max(1, static_cast<int>(usableH / lineHeight_));
            overlay->resize(cols, rows);

            tab->pushOverlay(std::move(overlay));
            setNeedsRedraw();
            return true;
        };
        scbs.popOverlay = [this](Script::TabId tabId) {
            if (tabId < 0 || tabId >= static_cast<int>(tabs_.size())) return;
            Tab* tab = tabs_[tabId].get();
            if (tab->hasOverlay()) {
                tab->popOverlay();
                setNeedsRedraw();
            }
        };
        scbs.createTab = [this]() -> int {
            createTab();
            return activeTabIdx_;
        };
        scbs.closeTab = [this](int tabId) {
            closeTab(tabId);
        };
        scbs.panePopups = [this](Script::PaneId paneId) -> std::vector<Script::AppCallbacks::PopupInfo> {
            std::vector<Script::AppCallbacks::PopupInfo> result;
            for (auto& tab : tabs_) {
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
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    PlatformCallbacks pcbs;
                    pcbs.onTerminalExited = [](Terminal*) {};
                    pcbs.quit = [this]() { quit(); };
                    pcbs.onInput = std::move(onInput);
                    if (p->createPopup(popupId, x, y, w, h, std::move(pcbs))) {
                        // Initialize popup render state
                        std::string key = popupStateKey(paneId, popupId);
                        PaneRenderState& prs = popupRenderStates_[key];
                        size_t needed = static_cast<size_t>(w) * h;
                        prs.resolvedCells.resize(needed);
                        prs.rowShapingCache.resize(h);
                        prs.dirty = true;
                        // Dirty all popup render states for this pane on any content change.
                        // onPopupEvent is a single slot on Pane so we can't key it per popup;
                        // dirtying all of them is cheap and correct regardless of which fired.
                        p->onPopupEvent = [this, paneId]() {
                            for (auto& [key, prs] : popupRenderStates_) {
                                if (key.substr(0, key.find('/')) == std::to_string(paneId))
                                    prs.dirty = true;
                            }
                            setNeedsRedraw();
                        };
                        // Dirty parent pane so the popup composite entry is added
                        auto it = paneRenderStates_.find(paneId);
                        if (it != paneRenderStates_.end()) it->second.dirty = true;
                        setNeedsRedraw();
                        return true;
                    }
                    return false;
                }
            }
            return false;
        };
        scbs.destroyPopup = [this](Script::PaneId paneId, const std::string& popupId) {
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    bool wasPopupFocused = (p->focusedPopupId() == popupId);
                    p->destroyPopup(popupId);
                    Terminal* t = p->terminal();
                    if (wasPopupFocused && t && p->popups().empty())
                        t->focusEvent(true);
                    if (t) t->grid().markAllDirty();
                    // Release popup render state (deferred — texture may still be in flight)
                    std::string key = popupStateKey(paneId, popupId);
                    auto pit = popupRenderStates_.find(key);
                    if (pit != popupRenderStates_.end()) {
                        if (pit->second.heldTexture)
                            pendingTabBarRelease_.push_back(pit->second.heldTexture);
                        for (auto* tx : pit->second.pendingRelease)
                            pendingTabBarRelease_.push_back(tx);
                        popupRenderStates_.erase(pit);
                    }
                    // Dirty parent pane so it re-renders without the popup composite entry
                    auto it = paneRenderStates_.find(paneId);
                    if (it != paneRenderStates_.end()) it->second.dirty = true;
                    setNeedsRedraw();
                    return;
                }
            }
        };
        scbs.resizePopup = [this](Script::PaneId paneId, const std::string& popupId,
                                   int x, int y, int w, int h) -> bool {
            for (auto& tab : tabs_) {
                if (Pane* p = tab->layout()->pane(paneId)) {
                    if (p->resizePopup(popupId, x, y, w, h)) {
                        if (auto* t = p->terminal()) t->grid().markAllDirty();
                        // Resize popup render state
                        std::string key = popupStateKey(paneId, popupId);
                        auto pit = popupRenderStates_.find(key);
                        if (pit != popupRenderStates_.end()) {
                            PaneRenderState& prs = pit->second;
                            prs.resolvedCells.resize(static_cast<size_t>(w) * h);
                            prs.rowShapingCache.assign(h, {});
                            prs.dirty = true;
                            if (prs.heldTexture) {
                                prs.pendingRelease.push_back(prs.heldTexture);
                                prs.heldTexture = nullptr;
                            }
                        }
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
            for (auto& tab : tabs_) {
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
    actionDispatcher_.addListener([this](Action::TypeIndex idx, const Action::Any& action) {
        if (auto* sa = std::get_if<Action::ScriptAction>(&action)) {
            scriptEngine_.notifyAction(sa->name);
        } else {
            scriptEngine_.notifyAction(std::string(Action::nameOf(idx)));
        }
    });

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
    {
        std::string watchPath = configFilePath();
        if (!watchPath.empty() && eventLoop_) {
            eventLoop_->addFileWatch(watchPath, [this]() {
                if (configDebounceActive_) {
                    eventLoop_->removeTimer(configDebounceTimer_);
                    configDebounceActive_ = false;
                }
                configDebounceTimer_ = eventLoop_->addTimer(300, false, [this]() {
                    configDebounceActive_ = false;
                    reloadConfigNow();
                });
                configDebounceActive_ = true;
            });
        }
    }

    // Set up the per-iteration tick
    if (eventLoop_) {
        eventLoop_->onTick = [this]() {
            if (shouldClose()) {
                eventLoop_->stop();
                return;
            }

            // Advance progress animations
            {
                bool hasAnim = false;
                for (auto& t : tabs_) {
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

            scriptEngine_.executePendingJobs();
            device_.Tick();

            if (needsRedraw_ || (debugIPC_ && debugIPC_->pngScreenshotPending()))
                renderFrame();

            // On Vulkan/Linux, MapAsync callbacks only fire during device_.Tick().
            // Wakeup the event loop so it keeps ticking until all readbacks complete.
            if (pendingGpuCallbacks_ > 0)
                eventLoop_->wakeup();
        };
    }

    if (eventLoop_) eventLoop_->run();

    if (debugSink_) debugSink_->setIPC(nullptr);

    // Cleanup
    std::vector<int> fds;
    for (auto& [fd, _] : ptyPolls_) fds.push_back(fd);
    for (int fd : fds) removePtyPoll(fd);

    if (configDebounceActive_) {
        eventLoop_->removeTimer(configDebounceTimer_);
        configDebounceActive_ = false;
    }
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
    needsRedraw_ = true;
    if (eventLoop_) eventLoop_->wakeup();
}
