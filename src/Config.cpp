#include "Config.h"
#include "Resources.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glaze/toml.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <system_error>

static std::filesystem::path configDir() // internal
{
    // XDG_CONFIG_HOME on Linux; on macOS fall back to ~/.config (same XDG convention)
    const char *xdgConfig = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdgConfig && xdgConfig[0]) {
        base = xdgConfig;
    } else {
        const char *home = std::getenv("HOME");
        if (!home || !home[0]) {
            return {};
        }
        base = std::filesystem::path(home) / ".config";
    }
    return base / "MasterBandit";
}

static std::filesystem::path configPath() // internal
{
    auto d = configDir();
    return d.empty() ? std::filesystem::path {} : d / "config.toml";
}

static std::filesystem::path configJsPath() // internal
{
    auto d = configDir();
    return d.empty() ? std::filesystem::path {} : d / "config.js";
}

static std::filesystem::path configTsPath() // internal
{
    auto d = configDir();
    return d.empty() ? std::filesystem::path {} : d / "config.ts";
}

std::string configFilePath()
{
    auto p = configPath();
    return p.empty() ? std::string {} : p.string();
}

std::string configJsFilePath()
{
    auto p = configJsPath();
    return p.empty() ? std::string {} : p.string();
}

std::string configTsFilePath()
{
    auto p = configTsPath();
    return p.empty() ? std::string {} : p.string();
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

namespace {
// Overwrite `dest` with the contents of `source`, unless `dest` is a
// symlink (lstat semantics) — that signals the user is managing the file
// themselves (e.g. pointing at a source checkout) and we leave it alone.
// Best-effort: every failure mode is logged and swallowed so a broken
// scaffold never blocks startup.
void refreshOneScaffold(const std::filesystem::path &source,
                        const std::filesystem::path &dest)
{
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        spdlog::warn("Config: bundled file missing at {}; config.ts scaffolding incomplete",
                     source.string());
        return;
    }
    if (std::filesystem::is_symlink(dest, ec)) {
        spdlog::info("Config: {} is a symlink, leaving it alone", dest.string());
        return;
    }
    std::filesystem::copy_file(source,
                               dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        spdlog::warn("Config: failed to copy {} -> {}: {}",
                     source.string(),
                     dest.string(),
                     ec.message());
    }
}
} // namespace

void refreshTypesScaffolding()
{
    auto dir = configDir();
    if (dir.empty()) {
        return;
    }

    std::error_code ec;
    auto typesDir = dir / "types";
    std::filesystem::create_directories(typesDir, ec);
    if (ec) {
        spdlog::warn("Config: failed to create {}: {}", typesDir.string(), ec.message());
        return;
    }

    refreshOneScaffold(Resources::path("types/mb.d.ts"), typesDir / "mb.d.ts");
    refreshOneScaffold(Resources::path("types/tsconfig.json"), dir / "tsconfig.json");
}
