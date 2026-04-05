#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct JSRuntime;
struct JSContext;
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
    struct PaneInfo { int cols; int rows; std::string title; std::string cwd; bool hasPty; bool focused; std::string focusedPopupId; };
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

    // Load scripts
    InstanceId loadApplet(const std::string& path);
    InstanceId loadController(const std::string& path);
    void unload(InstanceId id);

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

    // Deliver input to listeners on registered objects across all contexts.
    // registryName is "__pane_registry" or "__overlay_registry".
    void deliverInput(const char* registryName, uint32_t key, const char* data, size_t len);
    // Deliver input to popup listeners (keyed by "paneId:popupId" string).
    void deliverPopupInput(const std::string& regKey, const char* data, size_t len);

    // Run pending JS jobs. Call from main loop.
    void executePendingJobs();

    // C callback trampoline access
    const AppCallbacks& callbacks() const { return callbacks_; }
    void addPaneOutputFilter(PaneId pane) { paneOutputFilterCount_[pane]++; }
    void addPaneInputFilter(PaneId pane) { paneInputFilterCount_[pane]++; }
    void addOverlayOutputFilter(TabId tab) { overlayOutputFilterCount_[tab]++; }
    void addOverlayInputFilter(TabId tab) { overlayInputFilterCount_[tab]++; }

    uint32_t nextTimer() { return nextTimerId_++; }

    struct Instance {
        InstanceId id;
        JSContext* ctx;
        enum class Type { Applet, Controller } type;
    };
    Instance* findInstanceByCtx(JSContext* ctx);

private:
    JSRuntime* rt_ = nullptr;
    uv_loop_t* loop_ = nullptr;
    std::vector<Instance> instances_;
    InstanceId nextId_ = 1;
    uint32_t nextTimerId_ = 1;
    AppCallbacks callbacks_;

    std::unordered_map<PaneId, int> paneOutputFilterCount_;
    std::unordered_map<PaneId, int> paneInputFilterCount_;
    std::unordered_map<TabId, int> overlayOutputFilterCount_;
    std::unordered_map<TabId, int> overlayInputFilterCount_;

    JSContext* createContext();
    void setupAppletGlobals(JSContext* ctx, InstanceId id);
    void setupControllerGlobals(JSContext* ctx, InstanceId id);
    Instance* findInstance(InstanceId id);

    bool runPaneFilters(PaneId pane, const char* filterProp, std::string& data);
    bool runOverlayFilters(TabId tab, const char* filterProp, std::string& data);

    void cleanupPane(PaneId pane);
    void cleanupTab(TabId tab);

    static std::string readFile(const std::string& path);
};

} // namespace Script
