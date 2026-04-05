#include "PlatformDawn.h"
#include "Log.h"
#include "NativeSurface.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>


int PlatformDawn::exec()
{
    Tab* tab = activeTab();
    if (!tab || tab->layout()->panes().empty()) return 1;

    running_ = true;
    loop_ = uv_default_loop();

    debugIPC_ = std::make_unique<DebugIPC>(loop_,
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

    scriptEngine_.setLoop(loop_);

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
                        isFocused, p->focusedPopupId()
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
            if (tab->hasOverlay()) return false; // only one overlay at a time

            TerminalCallbacks cbs;
            cbs.event = [this](TerminalEmulator*, int, void*) {
                needsRedraw_ = true;
            };

            PlatformCallbacks pcbs;
            pcbs.onTerminalExited = [](Terminal*) {}; // headless, no exit
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
            needsRedraw_ = true;
            return true;
        };
        scbs.popOverlay = [this](Script::TabId tabId) {
            if (tabId < 0 || tabId >= static_cast<int>(tabs_.size())) return;
            Tab* tab = tabs_[tabId].get();
            if (tab->hasOverlay()) {
                tab->popOverlay();
                needsRedraw_ = true;
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
                        p->onPopupEvent = [this, paneId]() {
                            auto it = paneRenderStates_.find(paneId);
                            if (it != paneRenderStates_.end()) it->second.dirty = true;
                            needsRedraw_ = true;
                        };
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
                    p->destroyPopup(popupId);
                    // Force re-resolve all rows so popup overlay cells are cleared
                    if (auto* t = p->terminal()) t->grid().markAllDirty();
                    auto it = paneRenderStates_.find(paneId);
                    if (it != paneRenderStates_.end()) it->second.dirty = true;
                    needsRedraw_ = true;
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
                        auto it = paneRenderStates_.find(paneId);
                        if (it != paneRenderStates_.end()) it->second.dirty = true;
                        needsRedraw_ = true;
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
                        needsRedraw_ = true;
                    }
                    return;
                }
            }
        };
        scriptEngine_.setCallbacks(std::move(scbs));
    }

    // Hook action dispatcher to notify script engine
    actionDispatcher_.addListener([this](Action::TypeIndex idx, const Action::Any& action) {
        // For ScriptAction, use the actual namespace.name instead of "ScriptAction"
        if (auto* sa = std::get_if<Action::ScriptAction>(&action)) {
            scriptEngine_.notifyAction(sa->name);
        } else {
            scriptEngine_.notifyAction(std::string(Action::nameOf(idx)));
        }
    });

    // Set up script engine config dir for allowlist persistence
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
        scriptEngine_.loadController(scriptsDir + "applet-loader.js");
        scriptEngine_.loadController(scriptsDir + "command-palette.js");
    }

    // Add PTY polls for all terminals already created
    for (auto& panePtr : tab->layout()->panes()) {
        Terminal* term = panePtr->terminal();
        if (term && term->masterFD() >= 0) {
            addPtyPoll(term->masterFD(), term);
        }
    }

    uv_idle_init(loop_, &idleCb_);
    idleCb_.data = this;
    uv_idle_start(&idleCb_, [](uv_idle_t* handle) {
        auto* self = static_cast<PlatformDawn*>(handle->data);
        if (!self->isHeadless()) {
            glfwPollEvents();

            if (self->shouldClose()) {
                uv_stop(self->loop_);
                return;
            }
        }

        // Advance progress animations
        {
            bool hasAnim = false;
            for (auto& tab : self->tabs_) {
                for (auto& panePtr : tab->layout()->panes()) {
                    if (panePtr->progressState() == 3) { hasAnim = true; break; }
                }
                if (hasAnim) break;
            }
            if (hasAnim) {
                // Indeterminate: redraw every frame for smooth animation
                self->needsRedraw_ = true;
                // Tab bar glyph animation at 10fps
                auto now = TerminalEmulator::mono();
                if (self->tabBarVisible() && now - self->lastAnimTick_ > 100) {
                    self->lastAnimTick_ = now;
                    self->tabBarAnimFrame_++;
                    self->tabBarDirty_ = true;
                }
            }
        }

        // Run pending JS microtasks
        self->scriptEngine_.executePendingJobs();

        self->device_.Tick();
        self->renderFrame();
    });

    uv_run(loop_, UV_RUN_DEFAULT);

    if (debugSink_) {
        debugSink_->setIPC(nullptr);
    }

    // Stop all PTY polls
    std::vector<int> fds;
    for (auto& [fd, _] : ptyPolls_) fds.push_back(fd);
    for (int fd : fds) removePtyPoll(fd);

    uv_idle_stop(&idleCb_);
    uv_close(reinterpret_cast<uv_handle_t*>(&idleCb_), nullptr);
    if (debugIPC_) debugIPC_->closeHandles();
    uv_run(loop_, UV_RUN_DEFAULT);
    debugIPC_.reset();

    return exitStatus_;
}


void PlatformDawn::quit(int status)
{
    exitStatus_ = status;
    if (loop_) {
        uv_stop(loop_);
    }
}

