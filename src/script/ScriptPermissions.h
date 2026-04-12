#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Script {

// Individual permission bits
enum Perm : uint32_t {
    None              = 0,
    // ui group
    UiOverlayCreate   = 1 << 0,
    UiOverlayClose    = 1 << 1,
    UiPopupCreate     = 1 << 2,
    UiPopupDestroy    = 1 << 3,
    // io group
    IoFilterInput     = 1 << 4,
    IoFilterOutput    = 1 << 5,
    IoInject          = 1 << 6,
    // shell group
    ShellWrite        = 1 << 7,
    ShellReadCommands = 1 << 16,  // read OSC 133 command records + subscribe to commandComplete
    // actions group
    ActionsInvoke     = 1 << 8,
    // tabs group
    TabsCreate        = 1 << 9,
    TabsClose         = 1 << 10,
    // scripts group
    ScriptsLoad       = 1 << 11,
    ScriptsUnload     = 1 << 12,
    // fs group
    FsRead            = 1 << 13,  // read from script dir and config dir
    FsWrite           = 1 << 14,  // read+write ~/.config/MasterBandit/<scriptname>/
    // net group
    NetListenLocal    = 1 << 15,  // bind WebSocket server on 127.0.0.1 or unix socket

    // Group masks
    GroupUi      = UiOverlayCreate | UiOverlayClose | UiPopupCreate | UiPopupDestroy,
    GroupIo      = IoFilterInput | IoFilterOutput | IoInject,
    GroupShell   = ShellWrite | ShellReadCommands,
    GroupActions = ActionsInvoke,
    GroupTabs    = TabsCreate | TabsClose,
    GroupScripts = ScriptsLoad | ScriptsUnload,
    GroupFs      = FsRead | FsWrite,
    GroupNet     = NetListenLocal,

    All          = 0xFFFFFFFF,
};

// Parse "ui,io,shell" or "ui,io.filter.input,shell" into a bitmask
uint32_t parsePermissions(const std::string& permStr);

// Convert bitmask back to a comma-separated group string
std::string permissionsToString(uint32_t perms);

// Map an action name (e.g. "NewTab") to additional required permission bits.
// Returns 0 if the action needs no extra permission beyond ActionsInvoke.
uint32_t actionPermission(const std::string& actionName);

// Compute SHA-256 hex digest of a string
std::string sha256Hex(const std::string& content);

// Bump when permission semantics change (new permissions, renamed groups, etc.)
// Mismatched version in the TOML file discards all cached entries.
inline constexpr int kAllowlistVersion = 4;

// Persistent allowlist/denylist for script permissions
class Allowlist {
public:
    void load(const std::string& configDir);
    void save() const;

    struct AllowEntry {
        std::string path;
        std::string sha256;
        uint32_t permissions = 0;
        // All .js files in the script's directory tree at approval time: {path, sha256}
        std::vector<std::pair<std::string, std::string>> modules;
    };

    // Returns the allow entry if path+hash match; nullptr otherwise.
    const AllowEntry* check(const std::string& path, const std::string& hash) const;
    bool isDenied(const std::string& path, const std::string& hash) const;

    void allow(const std::string& path, const std::string& hash,
               uint32_t permissions,
               const std::vector<std::pair<std::string, std::string>>& modules = {});
    void deny(const std::string& path, const std::string& hash);

private:
    struct DenyEntry { std::string path; std::string sha256; };

    std::string filePath_;
    std::vector<AllowEntry> allowed_;
    std::vector<DenyEntry> denied_;
};

} // namespace Script
