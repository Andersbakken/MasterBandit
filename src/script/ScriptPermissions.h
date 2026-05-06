#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Script {

// Individual permission bits
enum Perm : uint32_t {
    None              = 0,
    // ui group. Bits 0 and 1 are reserved (never asserted) to preserve the
    // numeric stability of the remaining ui.* bits against existing on-disk
    // allowlists — the mismatched kAllowlistVersion below would wipe them
    // anyway but this is belt-and-braces.
    UiPopupCreate     = 1 << 2,
    UiPopupDestroy    = 1 << 3,
    // Grab keyboard focus on a UI element (popup, embedded terminal, future
    // focusable widgets). Intentionally NOT in GroupUi so that creating /
    // managing popups (common) does not implicitly include focus hijacking
    // (rare and disruptive). User scripts must request "ui.focus" explicitly;
    // built-ins (Perm::All) get it for free.
    UiFocus           = 1 << 22,
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
    // clipboard group
    ClipboardRead     = 1 << 17,  // read system clipboard / primary selection
    ClipboardWrite    = 1 << 18,  // write system clipboard / primary selection
    // pane query group
    // Read access to pane document content: selection, cursor, scrollback
    // text extraction (getTextFromRows / getLinksFromRows / linkAt /
    // rowIdAt) and rowEvicted lifecycle events.
    PaneRead          = 1 << 19,
    // layout group — structural mutation of the UI tree (create/destroy nodes,
    // reparent, change active children, bind TabBars). Read-only introspection
    // (mb.layout.node, computeRects) is currently ungated.
    LayoutModify      = 1 << 20,
    // config group — runtime mutation of the in-memory Config struct via
    // mb.config.patch / addKeybinding / removeKeybinding / addMousebinding /
    // removeMousebinding. Read access to mb.config remains ungated. Last-write
    // wins against TOML hot-reload (a subsequent disk edit's applyConfig
    // overwrites JS patches; not persisted to disk).
    ConfigModify      = 1 << 21,
    // process group — launch external (non-PTY) processes via mb.process.spawn.
    // Materially different threat model from shell.write (which is bounded to
    // an existing pane's PTY) and io.inject (which only synthesises terminal
    // input): a script with this bit can run arbitrary binaries with the
    // user's environment. Held separate so popup-only / shell-only scripts
    // don't get it implicitly. Built-ins receive it via Perm::All.
    ProcessSpawn      = 1 << 23,

    // Group masks
    GroupUi      = UiPopupCreate | UiPopupDestroy,
    GroupIo      = IoFilterInput | IoFilterOutput | IoInject,
    GroupShell   = ShellWrite | ShellReadCommands,
    GroupActions = ActionsInvoke,
    GroupTabs    = TabsCreate | TabsClose,
    GroupScripts = ScriptsLoad | ScriptsUnload,
    GroupFs      = FsRead | FsWrite,
    GroupNet     = NetListenLocal,
    GroupClipboard = ClipboardRead | ClipboardWrite,
    GroupLayout    = LayoutModify,
    GroupConfig    = ConfigModify,
    GroupProcess   = ProcessSpawn,

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
inline constexpr int kAllowlistVersion = 11;

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
