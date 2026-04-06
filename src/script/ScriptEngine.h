#pragma once

#include "ScriptPermissions.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct JSRuntime;
struct JSContext;
struct JSValue;
struct uv_loop_s;
typedef struct uv_loop_s uv_loop_t;

namespace Script {

using InstanceId = uint64_t;
using PaneId = int;
using TabId = int;

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
    // Invoke an action by name
    std::function<bool(const std::string&, const std::vector<std::string>&)> invokeAction;
    // Query pane info
    struct PaneInfo { int cols; int rows; std::string title; std::string cwd; bool hasPty; bool focused; std::string focusedPopupId; std::string foregroundProcess; };
    std::function<PaneInfo(PaneId)> paneInfo;
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
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void setCallbacks(AppCallbacks cbs);
    void setLoop(uv_loop_t* loop) { loop_ = loop; }
    uv_loop_t* loop() const { return loop_; }

    // Set config directory for allowlist persistence
    void setConfigDir(const std::string& dir);

    // Set the directory containing built-in JS modules (mb:tui, etc.)
    void setBuiltinModulesDir(const std::string& dir) { builtinModulesDir_ = dir; }
    const std::string& builtinModulesDir() const { return builtinModulesDir_; }
    const std::string& configDir() const { return configDir_; }

    // Load a built-in script (fully trusted, all permissions)
    InstanceId loadController(const std::string& path);

    // Load a user script with declared permissions.
    // Returns instanceId if pre-approved, 0 if pending user prompt or denied.
    InstanceId loadScript(const std::string& path, uint32_t requestedPerms);

    void unload(InstanceId id);

    // Called by JS (applet-loader) when user responds to permission prompt
    void approveScript(const std::string& path, char response);

    // --- Synchronous filters (called from PTY read / input path) ---
    bool filterPaneOutput(PaneId pane, std::string& data);
    bool filterPaneInput(PaneId pane, std::string& data);
    bool filterOverlayOutput(TabId tab, std::string& data);
    bool filterOverlayInput(TabId tab, std::string& data);

    bool hasPaneOutputFilters(PaneId pane) const;
    bool hasPaneInputFilters(PaneId pane) const;
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
    void addOverlayOutputFilter(TabId tab, InstanceId instId);
    void addOverlayInputFilter(TabId tab, InstanceId instId);

    uint32_t nextTimer() { return nextTimerId_++; }

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

private:
    JSRuntime* rt_ = nullptr;
    uv_loop_t* loop_ = nullptr;
    std::deque<Instance> instances_; // deque for pointer stability
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

    std::unordered_map<PaneId, int> paneOutputFilterCount_;
    std::unordered_map<PaneId, int> paneInputFilterCount_;
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

    static std::string readFile(const std::string& path);
};

} // namespace Script
