#pragma once

#include "ScriptPermissions.h"

#include <quickjs.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <eventloop/EventLoop.h>

struct JSRuntime;
struct JSContext;

namespace Script {

using InstanceId = uint64_t;
using PaneId = int;
using TabId = int;

// OSC 133 command record as surfaced to scripts — shared between AppCallbacks
// (for pane.commands / lastCommand queries) and Engine::notifyCommandComplete
// (for the event payload). Kept at namespace scope so forward declarations work.
struct CommandInfo {
    uint64_t id = 0;
    std::string cwd;
    std::optional<int> exitCode;
    uint64_t startMs = 0;
    uint64_t endMs = 0;
    // Stable row IDs for lazy text extraction via paneGetText/overlayGetText.
    uint64_t promptStartRowId = 0;
    uint64_t commandStartRowId = 0;
    uint64_t outputStartRowId = 0;
    uint64_t outputEndRowId = 0;
    // Volatile abs rows at query time (for JS position properties).
    int promptStartAbsRow = -1, promptStartCol = -1;
    int commandStartAbsRow = -1, commandStartCol = -1;
    int outputStartAbsRow = -1, outputStartCol = -1;
    int outputEndAbsRow = -1, outputEndCol = -1;
};

struct AppCallbacks {
    // Inject data directly into a terminal emulator (bypass PTY)
    std::function<void(PaneId, const std::string&)> injectPaneData;
    std::function<void(TabId, const std::string&)> injectOverlayData;
    // Write to PTY master fd (shell stdin)
    std::function<void(PaneId, const std::string&)> writePaneToShell;
    std::function<void(TabId, const std::string&)> writeOverlayToShell;
    // Check if a PTY exists
    std::function<bool(PaneId)> paneHasPty;
    std::function<bool(TabId)> overlayHasPty;
    // Check if there is an active tab
    std::function<bool()> hasActiveTab;
    // Invoke an action by name
    std::function<bool(const std::string&, const std::vector<std::string>&)> invokeAction;
    // Query pane info
    struct PaneInfo {
        int cols; int rows; std::string title; std::string cwd;
        bool hasPty; bool focused; std::string focusedPopupId; std::string foregroundProcess;
        bool hasSelection = false;
        uint64_t selectionStartRowId = 0; int selectionStartCol = 0;
        uint64_t selectionEndRowId = 0;   int selectionEndCol = 0;
        uint64_t cursorRowId = 0; int cursorCol = 0;
        uint64_t oldestRowId = 0; uint64_t newestRowId = 0;
        bool mouseInPane = false;
        int mouseCellX = 0; int mouseCellY = 0;
        int mousePixelX = 0; int mousePixelY = 0;
    };
    std::function<PaneInfo(PaneId)> paneInfo;
    // Query OSC 133 command records for a pane. Returns most-recent-last, up to `limit`
    // entries (0 = all). Used by pane.commands / pane.lastCommand JS properties.
    std::function<std::vector<CommandInfo>(PaneId, int limit)> paneCommands;
    // Extract plain text from a row-id range in a pane's or overlay's document.
    // startCol/endCol bound which columns are included on the first/last row;
    // pass 0 and INT_MAX (or std::numeric_limits<int>::max()) for full rows.
    std::function<std::string(PaneId, uint64_t startRowId, int startCol,
                              uint64_t endRowId, int endCol)> paneGetText;
    std::function<std::string(TabId, uint64_t startRowId, int startCol,
                              uint64_t endRowId, int endCol)> overlayGetText;
    // Returns the stable row ID for a screen row, or nullopt if out of range.
    std::function<std::optional<uint64_t>(PaneId, int screenRow)> paneRowIdAt;
    std::function<std::optional<uint64_t>(TabId, int screenRow)> overlayRowIdAt;
    // Query overlay info
    struct OverlayInfo { int cols; int rows; bool hasPty; bool exists; };
    std::function<OverlayInfo(TabId)> overlayInfo;
    // Query tab/pane structure
    struct TabInfo { TabId id; bool active; std::vector<PaneId> panes; PaneId focusedPane; bool hasOverlay; };
    std::function<std::vector<TabInfo>()> tabs;
    // Create a headless overlay on a tab. onInput is called when user types into it.
    std::function<bool(TabId, std::function<void(const char*, size_t)> onInput)> createOverlay;
    // Pop overlay from a tab.
    std::function<void(TabId)> popOverlay;
    // Create a new tab. Returns the tab index.
    std::function<int()> createTab;
    // Close a tab by index.
    std::function<void(int)> closeTab;
    // Query popups on a pane.
    struct PopupInfo { std::string id; int x; int y; int w; int h; bool focused; };
    std::function<std::vector<PopupInfo>(PaneId)> panePopups;
    // Create a popup on a pane. Returns false on failure.
    std::function<bool(PaneId, const std::string& id, int x, int y, int w, int h,
                       std::function<void(const char*, size_t)> onInput)> createPopup;
    // Destroy a popup on a pane.
    std::function<void(PaneId, const std::string& id)> destroyPopup;
    // Resize/move a popup on a pane.
    std::function<bool(PaneId, const std::string& id, int x, int y, int w, int h)> resizePopup;
    // Inject data into a popup's terminal.
    std::function<void(PaneId, const std::string& id, const std::string& data)> injectPopupData;
    // Clipboard access. source is "clipboard" or "primary".
    std::function<std::string(const std::string& source)> getClipboard;
    std::function<void(const std::string& source, const std::string& text)> setClipboard;
    // URL at a cell position (returns empty string if none).
    std::function<std::string(PaneId, uint64_t rowId, int col)> paneUrlAt;
    // Hyperlinks within a row-id range.
    struct LinkInfo {
        std::string url;
        uint64_t startRowId; int startCol;
        uint64_t endRowId;   int endCol;
    };
    std::function<std::vector<LinkInfo>(PaneId, uint64_t startRowId, uint64_t endRowId, int limit)> paneGetLinksFromRows;
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setCallbacks(AppCallbacks cbs);
    void setLoop(EventLoop* loop) { loop_ = loop; }
    EventLoop* loop() const { return loop_; }

    // Set config directory for allowlist persistence
    void setConfigDir(const std::string& dir);

    // Set the directory containing built-in JS modules (mb:tui, etc.)
    void setBuiltinModulesDir(const std::string& dir) { builtinModulesDir_ = dir; }
    const std::string& builtinModulesDir() const { return builtinModulesDir_; }
    const std::string& configDir() const { return configDir_; }

    // Load a built-in script (fully trusted, all permissions)
    InstanceId loadController(const std::string& path);

    struct LoadResult {
        enum class Status { Loaded, Pending, Denied, Error };
        Status status = Status::Error;
        InstanceId id = 0;
        std::string error;
    };

    // Load a user script with declared permissions.
    //  - Loaded  → `id` is the running instance id.
    //  - Pending → user prompt raised; a matching `approveScript` call will follow.
    //  - Denied  → permanently denied per allowlist.
    //  - Error   → file unreadable or JS eval failed; `error` holds a message.
    LoadResult loadScript(const std::string& path, uint32_t requestedPerms);

    void unload(InstanceId id);

    // Called by JS (applet-loader) when user responds to permission prompt.
    // Returns the outcome so the caller can inform the originating channel
    // (e.g. applet-loader writes the shell ack after the user picks).
    LoadResult approveScript(const std::string& path, char response);

    // Custom XTGETTCAP capabilities registered by scripts.
    // Returns nullopt if the name is not registered.
    std::optional<std::string> lookupCustomTcap(const std::string& name) const;
    void registerTcap(const std::string& name, const std::string& value);
    void unregisterTcap(const std::string& name);

    // --- Synchronous filters (called from PTY read / input path) ---
    bool filterPaneOutput(PaneId pane, std::string& data);
    bool filterPaneInput(PaneId pane, std::string& data);
    bool filterOverlayOutput(TabId tab, std::string& data);
    bool filterOverlayInput(TabId tab, std::string& data);

    bool hasPaneOutputFilters(PaneId pane) const;
    bool hasPaneInputFilters(PaneId pane) const;
    bool hasPaneMouseMoveListeners(PaneId pane) const;
    bool hasOverlayOutputFilters(TabId tab) const;
    bool hasOverlayInputFilters(TabId tab) const;

    // --- Async events (enqueued as microtasks) ---
    void notifyAction(const std::string& actionName);
    void notifyPaneCreated(TabId tab, PaneId pane);
    void notifyPaneDestroyed(PaneId pane);
    void notifyTabCreated(TabId tab);
    void notifyTabDestroyed(TabId tab);
    void notifyPaneResized(PaneId pane, int cols, int rows);
    void notifyOverlayCreated(TabId tab);
    void notifyOverlayDestroyed(TabId tab);
    void notifyOSC(PaneId pane, int oscNum, const std::string& payload);
    void notifyForegroundProcessChanged(PaneId pane, const std::string& processName);
    void notifyPaneFocusChanged(PaneId pane, bool focused);
    void notifyFocusedPopupChanged(PaneId pane, const std::string& popupId);
    void notifyPaneMouseMove(PaneId pane, int cellX, int cellY, int pixelX, int pixelY);

    void notifyCommandComplete(PaneId pane, const CommandInfo& rec);

    // Deliver input to listeners on registered objects across all contexts.
    void deliverInput(const char* registryName, uint32_t key, const char* data, size_t len);
    // Deliver input to popup listeners (keyed by "paneId:popupId" string).
    void deliverPopupInput(const std::string& regKey, const char* data, size_t len);

    // Deliver mouse events to JS listeners.
    void deliverPopupMouseEvent(PaneId pane, const std::string& popupId,
                                 const std::string& type, int cellX, int cellY,
                                 int pixelX, int pixelY, int button);
    void deliverPaneMouseEvent(PaneId pane, const std::string& type,
                                int cellX, int cellY, int pixelX, int pixelY, int button);
    void deliverOverlayMouseEvent(TabId tab, const std::string& type,
                                   int cellX, int cellY, int pixelX, int pixelY, int button);

    // Run pending JS jobs. Call from main loop.
    void executePendingJobs();

    // C callback trampoline access
    const AppCallbacks& callbacks() const { return callbacks_; }
    void addPaneOutputFilter(PaneId pane, InstanceId instId);
    void addPaneInputFilter(PaneId pane, InstanceId instId);
    void addPaneMouseMoveListener(PaneId pane, InstanceId instId);
    void addOverlayOutputFilter(TabId tab, InstanceId instId);
    void addOverlayInputFilter(TabId tab, InstanceId instId);

    uint32_t nextTimer() { return nextTimerId_++; }

    // JS timer registry (setTimeout/setInterval → EventLoop::TimerId)
    struct JsTimer {
        JSContext*         ctx;
        JSValue            callback;
        EventLoop::TimerId loopId;
        bool               interval;
        uint64_t           ms;
    };
    std::unordered_map<uint32_t, JsTimer>& jsTimers() { return jsTimers_; }

    struct Instance {
        InstanceId id;
        JSContext* ctx;
        std::string path;
        std::string contentHash;
        uint32_t permissions = Perm::All;
        bool builtIn = false;
        std::string ns; // namespace set via mb.setNamespace()

        // Resources owned by this instance (cleaned up on unload)
        struct PopupRef { PaneId pane; std::string popupId; };
        std::vector<PopupRef> ownedPopups;
        std::vector<TabId> ownedOverlays;
        std::vector<PaneId> paneOutputFilters; // panes with output filters from this instance
        std::vector<PaneId> paneInputFilters;
        std::vector<PaneId> paneMouseMoveListeners;
        std::vector<TabId> overlayOutputFilters;
        std::vector<TabId> overlayInputFilters;
    };
    Instance* findInstanceByCtx(JSContext* ctx);
    Instance* findInstance(InstanceId id);

    // Script action registration
    bool setNamespace(InstanceId id, const std::string& ns);
    bool registerAction(InstanceId id, const std::string& name);
    bool isActionRegistered(const std::string& fullName) const;
    const std::set<std::string>& registeredActions() const { return registeredActions_; }

    // Re-entrancy guard for iterating instances_ while calling JS.
    // While iterating, unload() nulls ctx instead of erasing. When the
    // outermost guard destructs, deferred cleanups run and dead entries
    // are swept.
    bool iterating() const { return iterDepth_ > 0; }
    void deferCleanup(std::function<void(Engine*)> fn) { deferredCleanups_.push_back(std::move(fn)); }

    struct IterGuard {
        Engine* eng;
        IterGuard(Engine* e) : eng(e) { ++eng->iterDepth_; }
        ~IterGuard() {
            if (--eng->iterDepth_ == 0) {
                // Run registered cleanups
                auto fns = std::move(eng->deferredCleanups_);
                eng->deferredCleanups_.clear();
                for (auto& fn : fns) fn(eng);
                // Sweep dead instances (ctx nulled by deferred unload)
                std::erase_if(eng->instances_, [](const Instance& i) { return !i.ctx; });
            }
        }
        IterGuard(const IterGuard&) = delete;
        IterGuard& operator=(const IterGuard&) = delete;
    };

private:
    JSRuntime* rt_ = nullptr;
    EventLoop* loop_ = nullptr;
    std::deque<Instance> instances_; // deque for pointer stability
    int iterDepth_ = 0; // >0 while iterating instances_ and calling JS
    std::vector<std::function<void(Engine*)>> deferredCleanups_;
    InstanceId nextId_ = 1;
    uint32_t nextTimerId_ = 1;
    AppCallbacks callbacks_;
    std::string builtinModulesDir_;

    // Permission system
    Allowlist allowlist_;
    std::string configDir_;

    struct PendingScript {
        std::string path;
        std::string content;
        std::string hash;
        uint32_t requestedPerms;
        std::string popupId;
        PaneId promptPaneId;
    };
    std::unordered_map<std::string, PendingScript> pendingScripts_;

    std::set<std::string> registeredActions_; // "namespace.action" strings
    std::unordered_map<std::string, std::string> customTcaps_; // XTGETTCAP name → value

    std::unordered_map<uint32_t, JsTimer> jsTimers_;

    std::unordered_map<PaneId, int> paneOutputFilterCount_;
    std::unordered_map<PaneId, int> paneInputFilterCount_;
    std::unordered_map<PaneId, int> paneMouseMoveCount_;
    std::unordered_map<TabId, int> overlayOutputFilterCount_;
    std::unordered_map<TabId, int> overlayInputFilterCount_;

    JSContext* createContext();
    void setupGlobals(JSContext* ctx, InstanceId id);

    InstanceId loadScriptInternal(const std::string& path, const std::string& content,
                                   uint32_t permissions);
    void notifyPermissionRequired(const std::string& path, const std::string& permissions,
                                   const std::string& hash);

    void deliverMouseToRegistry(const char* registryName, uint32_t key,
                                 const std::string& type, int cellX, int cellY,
                                 int pixelX, int pixelY, int button);

    bool runPaneFilters(PaneId pane, const char* filterProp, std::string& data);
    bool runOverlayFilters(TabId tab, const char* filterProp, std::string& data);

    void cleanupPane(PaneId pane);
    void cleanupTab(TabId tab);

};

} // namespace Script
