#include "Config.h"
#include <glaze/toml.hpp>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::filesystem::path configDir()  // internal
{
    // XDG_CONFIG_HOME on Linux; on macOS fall back to ~/.config (same XDG convention)
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdgConfig && xdgConfig[0]) {
        base = xdgConfig;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !home[0]) return {};
        base = std::filesystem::path(home) / ".config";
    }
    return base / "MasterBandit";
}

static std::filesystem::path configPath()  // internal
{
    auto d = configDir();
    return d.empty() ? std::filesystem::path{} : d / "config.toml";
}

static std::filesystem::path configJsPath()  // internal
{
    auto d = configDir();
    return d.empty() ? std::filesystem::path{} : d / "config.js";
}

std::string configFilePath()
{
    auto p = configPath();
    return p.empty() ? std::string{} : p.string();
}

std::string configJsFilePath()
{
    auto p = configJsPath();
    return p.empty() ? std::string{} : p.string();
}

Config loadConfig()
{
    Config cfg;

    auto path = configPath();
    if (path.empty()) {
        spdlog::warn("Config: could not determine config path");
        return cfg;
    }

    // Ensure the parent dir exists so watchers can observe the file
    // appearing later, but do NOT create an empty config.toml. The
    // file-watch backends used in production (epoll on Linux, FSEvents
    // on macOS) watch the parent directory and fire on IN_CREATE /
    // FSEvents-create, so a missing file at startup is fine — saving
    // it later triggers the watch and reloadNow picks it up.
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ifstream f(path);
    if (!f) {
        return cfg;
    }

    std::string buf(std::istreambuf_iterator<char>(f), {});
    auto err = glz::read_toml(cfg, buf);
    if (err) {
        spdlog::warn("Config: failed to parse {}: {}", path.string(), glz::format_error(err, buf));
    } else {
        spdlog::info("Config: loaded {}", path.string());
    }

    return cfg;
}
