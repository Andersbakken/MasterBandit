#include "Config.h"
#include <glaze/toml.hpp>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static std::filesystem::path configPath()  // internal

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
    return base / "MasterBandit" / "config.toml";
}

std::string configFilePath()
{
    auto p = configPath();
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

    std::ifstream f(path);
    if (!f) {
        // No config file is normal on first run
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
