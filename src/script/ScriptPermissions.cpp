#include "ScriptPermissions.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

namespace Script {

// ============================================================================
// Permission parsing
// ============================================================================

static const std::unordered_map<std::string, uint32_t> kPermNames = {
    // Groups
    {"ui",          Perm::GroupUi},
    {"io",          Perm::GroupIo},
    {"shell",       Perm::GroupShell},
    {"actions",     Perm::GroupActions},
    {"tabs",        Perm::GroupTabs},
    {"scripts",     Perm::GroupScripts},
    {"fs",          Perm::GroupFs},
    // Individual
    {"ui.overlay.create", Perm::UiOverlayCreate},
    {"ui.overlay.close",  Perm::UiOverlayClose},
    {"ui.popup.create",   Perm::UiPopupCreate},
    {"ui.popup.destroy",  Perm::UiPopupDestroy},
    {"io.filter.input",   Perm::IoFilterInput},
    {"io.filter.output",  Perm::IoFilterOutput},
    {"io.inject",         Perm::IoInject},
    {"shell.write",       Perm::ShellWrite},
    {"actions.invoke",    Perm::ActionsInvoke},
    {"tabs.create",       Perm::TabsCreate},
    {"tabs.close",        Perm::TabsClose},
    {"scripts.load",      Perm::ScriptsLoad},
    {"scripts.unload",    Perm::ScriptsUnload},
    {"fs.read",           Perm::FsRead},
    {"fs.write",          Perm::FsWrite},
};

uint32_t parsePermissions(const std::string& permStr)
{
    if (permStr.empty()) return Perm::None;

    uint32_t result = 0;
    std::istringstream ss(permStr);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim whitespace
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        if (token.empty()) continue;

        auto it = kPermNames.find(token);
        if (it != kPermNames.end()) {
            result |= it->second;
        } else {
            spdlog::warn("ScriptPermissions: unknown permission '{}'", token);
        }
    }
    return result;
}

// Reverse mapping: emit the most compact group representation
std::string permissionsToString(uint32_t perms)
{
    if (perms == Perm::All) return "all";
    if (perms == Perm::None) return "none";

    std::string result;
    auto append = [&](const char* name) {
        if (!result.empty()) result += ',';
        result += name;
    };

    // Try groups first, then individual bits for leftovers
    struct GroupDef { uint32_t mask; const char* name; };
    static const GroupDef groups[] = {
        {Perm::GroupUi,      "ui"},
        {Perm::GroupIo,      "io"},
        {Perm::GroupShell,   "shell"},
        {Perm::GroupActions, "actions"},
        {Perm::GroupTabs,    "tabs"},
        {Perm::GroupScripts, "scripts"},
        {Perm::GroupFs,      "fs"},
    };

    uint32_t remaining = perms;
    for (const auto& g : groups) {
        if ((remaining & g.mask) == g.mask) {
            append(g.name);
            remaining &= ~g.mask;
        }
    }

    // Emit any leftover individual bits
    struct BitDef { uint32_t bit; const char* name; };
    static const BitDef bits[] = {
        {Perm::UiOverlayCreate, "ui.overlay.create"},
        {Perm::UiOverlayClose,  "ui.overlay.close"},
        {Perm::UiPopupCreate,   "ui.popup.create"},
        {Perm::UiPopupDestroy,  "ui.popup.destroy"},
        {Perm::IoFilterInput,   "io.filter.input"},
        {Perm::IoFilterOutput,  "io.filter.output"},
        {Perm::IoInject,        "io.inject"},
        {Perm::ShellWrite,      "shell.write"},
        {Perm::ActionsInvoke,   "actions.invoke"},
        {Perm::TabsCreate,      "tabs.create"},
        {Perm::TabsClose,       "tabs.close"},
        {Perm::ScriptsLoad,     "scripts.load"},
        {Perm::ScriptsUnload,   "scripts.unload"},
        {Perm::FsRead,          "fs.read"},
        {Perm::FsWrite,         "fs.write"},
    };
    for (const auto& b : bits) {
        if (remaining & b.bit) {
            append(b.name);
            remaining &= ~b.bit;
        }
    }

    return result;
}

// ============================================================================
// Action → permission mapping
// ============================================================================

uint32_t actionPermission(const std::string& actionName)
{
    // Actions that create or destroy resources need additional permissions
    if (actionName == "NewTab" || actionName == "SplitPane")
        return Perm::TabsCreate;
    if (actionName == "CloseTab" || actionName == "ClosePane")
        return Perm::TabsClose;
    if (actionName == "PushOverlay")
        return Perm::UiOverlayCreate;
    if (actionName == "PopOverlay")
        return Perm::UiOverlayClose;
    return 0; // safe actions need no extra permission
}

// ============================================================================
// SHA-256
// ============================================================================

std::string sha256Hex(const std::string& content)
{
    unsigned char hash[32];

#ifdef __APPLE__
    CC_SHA256(content.data(), static_cast<CC_LONG>(content.size()), hash);
#else
    SHA256(reinterpret_cast<const unsigned char*>(content.data()),
           content.size(), hash);
#endif

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; ++i) {
        result += hex[hash[i] >> 4];
        result += hex[hash[i] & 0x0F];
    }
    return result;
}

// ============================================================================
// Allowlist persistence
// ============================================================================

void Allowlist::load(const std::string& configDir)
{
    namespace fs = std::filesystem;

    filePath_ = (fs::path(configDir) / "allowed-scripts.toml").string();
    allowed_.clear();
    denied_.clear();

    std::ifstream f(filePath_);
    if (!f) return; // no file yet — normal on first run

    // Simple TOML parser for our specific format:
    //   [[allow]]
    //   path = "..."
    //   sha256 = "..."
    //   permissions = "ui,io,shell"
    //
    //   [[deny]]
    //   path = "..."
    //   sha256 = "..."

    // Check version first
    int fileVersion = 0;
    std::string line;
    AllowEntry curAllow;
    DenyEntry curDeny;
    bool inAllow = false, inDeny = false;

    auto flushAllow = [&]() {
        if (inAllow && !curAllow.path.empty())
            allowed_.push_back(std::move(curAllow));
        curAllow = {};
        inAllow = false;
    };
    auto flushDeny = [&]() {
        if (inDeny && !curDeny.path.empty())
            denied_.push_back(std::move(curDeny));
        curDeny = {};
        inDeny = false;
    };

    auto unquote = [](const std::string& s) -> std::string {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return s.substr(1, s.size() - 2);
        return s;
    };

    while (std::getline(f, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        if (line.empty() || line[0] == '#') continue;

        // Parse top-level version
        if (!inAllow && !inDeny && line.substr(0, 7) == "version") {
            auto eq2 = line.find('=');
            if (eq2 != std::string::npos) {
                std::string v = line.substr(eq2 + 1);
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                try { fileVersion = std::stoi(v); } catch (...) {}
            }
            if (fileVersion != kAllowlistVersion) {
                spdlog::warn("ScriptPermissions: version mismatch (file={}, expected={}), discarding cached entries",
                             fileVersion, kAllowlistVersion);
                return; // discard all entries
            }
            continue;
        }

        if (line == "[[allow]]") {
            flushAllow();
            flushDeny();
            inAllow = true;
            continue;
        }
        if (line == "[[deny]]") {
            flushAllow();
            flushDeny();
            inDeny = true;
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim key and val
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        val = unquote(val);

        if (inAllow) {
            if (key == "path") curAllow.path = val;
            else if (key == "sha256") curAllow.sha256 = val;
            else if (key == "permissions") curAllow.permissions = parsePermissions(val);
            else if (key.substr(0, 7) == "module_") {
                // "module_N = /abs/path.js|sha256hex"
                auto sep = val.rfind('|');
                if (sep != std::string::npos)
                    curAllow.modules.emplace_back(val.substr(0, sep), val.substr(sep + 1));
            }
        } else if (inDeny) {
            if (key == "path") curDeny.path = val;
            else if (key == "sha256") curDeny.sha256 = val;
        }
    }
    flushAllow();
    flushDeny();

    spdlog::info("ScriptPermissions: loaded {} allow, {} deny entries from {}",
                 allowed_.size(), denied_.size(), filePath_);
}

void Allowlist::save() const
{
    if (filePath_.empty()) return;

    // Ensure directory exists
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(filePath_).parent_path());

    std::ofstream f(filePath_);
    if (!f) {
        spdlog::error("ScriptPermissions: failed to write {}", filePath_);
        return;
    }

    f << "version = " << kAllowlistVersion << "\n\n";

    for (const auto& e : allowed_) {
        f << "[[allow]]\n";
        f << "path = \"" << e.path << "\"\n";
        f << "sha256 = \"" << e.sha256 << "\"\n";
        f << "permissions = \"" << permissionsToString(e.permissions) << "\"\n";
        for (size_t i = 0; i < e.modules.size(); ++i)
            f << "module_" << i << " = \"" << e.modules[i].first << "|" << e.modules[i].second << "\"\n";
        f << "\n";
    }
    for (const auto& e : denied_) {
        f << "[[deny]]\n";
        f << "path = \"" << e.path << "\"\n";
        f << "sha256 = \"" << e.sha256 << "\"\n\n";
    }
}

const Allowlist::AllowEntry* Allowlist::check(const std::string& path,
                                               const std::string& hash) const
{
    for (const auto& e : allowed_) {
        if (e.path == path && e.sha256 == hash)
            return &e;
    }
    return nullptr;
}

bool Allowlist::isDenied(const std::string& path, const std::string& hash) const
{
    for (const auto& e : denied_) {
        if (e.path == path && e.sha256 == hash)
            return true;
    }
    return false;
}

void Allowlist::allow(const std::string& path, const std::string& hash,
                       uint32_t permissions,
                       const std::vector<std::pair<std::string, std::string>>& modules)
{
    for (auto& e : allowed_) {
        if (e.path == path) {
            e.sha256 = hash;
            e.permissions = permissions;
            e.modules = modules;
            return;
        }
    }
    allowed_.push_back({path, hash, permissions, modules});
}

void Allowlist::deny(const std::string& path, const std::string& hash)
{
    for (auto& e : denied_) {
        if (e.path == path) {
            e.sha256 = hash;
            return;
        }
    }
    denied_.push_back({path, hash});
}

} // namespace Script
