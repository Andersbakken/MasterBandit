#pragma once

#include "Config.h"

#include <eventloop/EventLoop.h>

#include <functional>
#include <string>

// Owns the config hot-reload entry points and debounce wiring. On file-watch
// events it coalesces into a single reload after a short delay, then calls
// host_.applyConfig(Config) on the main thread. The actual mutation of
// PlatformDawn state stays on PlatformDawn because every field touched by a
// reload (font metrics, colors, tints, keybindings routed through
// InputController, tab-bar config, etc.) is read by multiple subsystems;
// ConfigLoader only owns the debounce timer + file watch + parse trigger.
class ConfigLoader {
public:
    struct Host {
        EventLoop* eventLoop = nullptr;
        // Called on the main thread after a successful parse. The host
        // acquires whatever locks are needed and applies the config.
        std::function<void(const Config&)> applyConfig;
    };

    ConfigLoader() = default;
    ~ConfigLoader();

    ConfigLoader(const ConfigLoader&) = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;

    void setHost(Host host) { host_ = std::move(host); }

    // Install a file watcher on the config file. 300ms debounce before a
    // parse is triggered. Safe to call only after the event loop exists.
    void installFileWatch(const std::string& path);

    // Parse the config file now and call host_.applyConfig.
    void reloadNow();

    // Cancel any pending debounce timer. Called during shutdown before the
    // event loop is torn down.
    void stop();

private:
    Host host_;
    EventLoop::TimerId debounceTimer_ = 0;
    bool debounceActive_ = false;
};
