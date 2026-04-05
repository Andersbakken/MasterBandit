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
        [this](int id) { return statsJson(id); });
    if (debugSink_) {
        debugSink_->setIPC(debugIPC_.get());
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

