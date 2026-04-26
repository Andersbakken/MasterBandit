#pragma once

#include "LayoutTree.h"
#include "ScriptPermissions.h"
#include "Uuid.h"

#include <quickjs.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <eventloop/EventLoop.h>

struct JSRuntime;
struct JSContext;
class Terminal;

namespace Script {

using InstanceId = uint64_t;
// Panes are identified by their LayoutTree node Uuid. The old integer paneId
// (monotonic counter that bridged render/input/script surfaces) is gone —
// nothing outside the Terminal itself uses it.
using PaneId = Uuid;
using TabId = int;

// OSC 133 command record as surfaced to scripts — shared between AppCallbacks
// (for pane.commands / pane.selectedCommand queries) and
// Engine::notifyCommandComplete (for the event payload). Kept at namespace
// scope so forward declarations work.
struct CommandInfo {
    uint64_t id = 0;
    std::string cwd;
    std::optional<int> exitCode;
    uint64_t startMs = 0;
    uint64_t endMs = 0;
    // Stable logical-line IDs for lazy text extraction.
    uint64_t promptStartLineId = 0;
    uint64_t commandStartLineId = 0;
    uint64_t outputStartLineId = 0;
    uint64_t outputEndLineId = 0;
    // Volatile abs rows at query time (for JS position properties).
    int promptStartAbsRow = -1, promptStartCol = -1;
    int commandStartAbsRow = -1, commandStartCol = -1;
    int outputStartAbsRow = -1, outputStartCol = -1;
    int outputEndAbsRow = -1, outputEndCol = -1;
};

struct AppCallbacks {
    // Request a render frame. The JS-side Terminal base class calls this
    // after inject / other mutators so changes become visible without
    // triple-dispatching through kind-specific callbacks.
    std::function<void()> requestRedraw;

    // Pixel dimensions of one cell at the current font/DPI. Window-global —
    // popups and embeddeds share the parent pane's font metrics.
    std::function<std::pair<float, float>()> fontCellSize;

    // Tab bar position from config — "top" | "bottom". Read by default-ui.js
    // to decide the root Container's child ordering. Config default is
    // "bottom".
    std::function<std::string()> tabBarPosition;
    // Write to PTY master fd (shell stdin) — raw bytes, no bracketing.
    std::function<void(PaneId, const std::string&)> writePaneToShell;
    // Paste to PTY master fd — wraps in \x1b[200~/\x1b[201~ when the terminal
    // currently has DECSET 2004 active (bracketed paste mode). Use for
    // content the user is pasting from elsewhere.
    std::function<void(PaneId, const std::string&)> pastePaneText;
    // Check if a PTY exists
    std::function<bool(PaneId)> paneHasPty;
    // Check if there is an active tab
    std::function<bool()> hasActiveTab;
    // Invoke an action by name
    std::function<bool(const std::string&, const std::vector<std::string>&)> invokeAction;
    // Query pane info
    struct PaneInfo {
        int cols; int rows; std::string title; std::string cwd;
        bool hasPty; bool focused; std::string focusedPopupId; std::string foregroundProcess;
        bool hasSelection = false;
        uint64_t selectionStartLineId = 0; int selectionStartCol = 0;
        uint64_t selectionEndLineId = 0;   int selectionEndCol = 0;
        uint64_t cursorLineId = 0; int cursorCol = 0;
        uint64_t oldestLineId = 0; uint64_t newestLineId = 0;
        bool mouseInPane = false;
        int mouseCellX = 0; int mouseCellY = 0;
        int mousePixelX = 0; int mousePixelY = 0;
        std::optional<uint64_t> selectedCommandId;
        // UUID string of this pane's Terminal node in the shared LayoutTree.
        // Empty when the Terminal isn't attached to a tree node (shouldn't
        // happen in production but stays empty safely).
        std::string nodeId;
    };
    std::function<PaneInfo(PaneId)> paneInfo;
    // Query OSC 133 command records for a pane. Returns most-recent-last, up to `limit`
    // entries (0 = all). Used by pane.commands / pane.selectedCommand JS properties.
    std::function<std::vector<CommandInfo>(PaneId, int limit)> paneCommands;
    // Set (or clear with nullopt) the pane's OSC 133 selected command.
    // Returns false if the command id is not present in the ring or the pane
    // is not found.
    std::function<bool(PaneId, std::optional<uint64_t>)> paneSetSelectedCommand;
    // Extract plain text from a row-id range in a pane's or overlay's document.
    // startCol/endCol bound which columns are included on the first/last row;
    // pass 0 and INT_MAX (or std::numeric_limits<int>::max()) for full rows.
    std::function<std::string(PaneId, uint64_t startRowId, int startCol,
                              uint64_t endRowId, int endCol)> paneGetText;
    // Returns the stable row ID for a screen row, or nullopt if out of range.
    // Resolve the logical-line id for a screen row position.
    std::function<std::optional<uint64_t>(PaneId, int screenRow)> paneLineIdAt;
    // Query tab/pane structure
    struct TabInfo {
        TabId id;
        bool active;
        std::vector<PaneId> panes;
        PaneId focusedPane;
        // UUID string of this Tab's root Container in the shared LayoutTree,
        // or empty when the tab has no tree representation.
        std::string nodeId;
    };
    std::function<std::vector<TabInfo>()> tabs;
    // Close a tab by index.
    std::function<void(int)> closeTab;

    // Synchronously kill a Terminal by its tree-node UUID. Extracts the
    // Terminal from the engine map, graveyards it, and fires the
    // `terminalExited` event. Tree node is left in place for JS to remove.
    // Returns true when a matching live Terminal was found and killed.
    std::function<bool(Uuid)> killTerminalByNodeId;

    // Quit the application. Trivial — wraps PlatformDawn's quit path.
    std::function<void()> quit;

    // JS-facing primitives. Each maps 1:1 to a PlatformDawn method; they're
    // wired through AppCallbacks so ScriptLayoutBindings stays decoupled
    // from the platform layer.
    struct NewPane { std::string nodeId; bool ok; };
    // Empty-tab variant. Returns {tabIdx, subtreeRoot uuid string}.
    std::function<std::pair<int, std::string>()> createEmptyTab;
    std::function<void(int)> activateTab;
    std::function<bool(Uuid)> focusPane;
    // Remove a tree node (Terminal leaf, Container, or Stack) from its
    // enclosing tab's subtree. Refuses if any descendant Terminal is still
    // live — the controller must killTerminal first.
    std::function<bool(Uuid)> removeNode;
    // Spawn a Terminal inside the Layout that owns `parentContainerNodeId`;
    // append the new Terminal node as the container's last child.
    std::function<NewPane(const std::string& parentContainerNodeId,
                          const std::string& cwd)> createTerminalInContainer;
    // Wrap `existingPaneNodeId` in a new Container and spawn a Terminal as
    // the sibling. `dir` is "horizontal" or "vertical".
    std::function<NewPane(const std::string& existingPaneNodeId,
                          const std::string& dir,
                          bool newIsFirst)> splitPaneByNodeId;
    // Apply a Stack zoom override. `stackNodeId` is the Stack whose rect
    // should be redirected; `targetOrEmpty == ""` clears the override.
    // Wraps LayoutTree::setStackZoom and then triggers a resize cascade
    // for the enclosing tab so terminals pick up the new rect.
    std::function<bool(const std::string& stackNodeId,
                       const std::string& targetNodeIdOrEmpty)> setStackZoom;
    // Move the boundary of `paneNodeId`'s slot by `amount` cells in `dir`.
    // `dir` is one of "left"/"right"/"up"/"down"; positive cells grow the
    // pane in that direction. Resolves pane → pixel delta via font metrics
    // native-side.
    std::function<bool(const std::string& paneNodeId,
                       const std::string& dir, int amount)> adjustPaneSize;
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

    // --- Embedded terminals (document-anchored inline surfaces) ---
    // Query embeddeds on a pane.
    struct EmbeddedInfo { uint64_t lineId; int rows; bool focused; };
    std::function<std::vector<EmbeddedInfo>(PaneId)> paneEmbeddeds;
    // Create an embedded terminal on a pane at the pane's current cursor row.
    // Returns the anchor lineId on success, 0 on failure (alt-screen, rows<=0,
    // duplicate anchor). Keystrokes routed into the focused embedded are
    // delivered to its JS "input" listeners by Platform via the regKey
    // "paneId:lineId" — no per-instance callback plumbing here.
    std::function<uint64_t(PaneId, int rows)> createEmbedded;
    // Destroy an embedded terminal by its anchor lineId.
    std::function<void(PaneId, uint64_t lineId)> destroyEmbedded;
    // Resize an embedded's row count (cols track parent cols automatically).
    std::function<bool(PaneId, uint64_t lineId, int rows)> resizeEmbedded;

    // Clipboard access. source is "clipboard" or "primary".
    std::function<std::string(const std::string& source)> getClipboard;
    std::function<void(const std::string& source, const std::string& text)> setClipboard;
    // URL at a cell position (returns empty string if none).
    std::function<std::string(PaneId, uint64_t lineId, int col)> paneUrlAt;
    // Hyperlinks within a logical-line range.
    struct LinkInfo {
        std::string url;
        uint64_t startLineId; int startCol;
        uint64_t endLineId;   int endCol;
    };
    std::function<std::vector<LinkInfo>(PaneId, uint64_t startLineId, uint64_t endLineId, int limit)> paneGetLinksFromRows;
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

    bool hasPaneOutputFilters(PaneId pane) const;
    bool hasPaneInputFilters(PaneId pane) const;
    bool hasPaneMouseMoveListeners(PaneId pane) const;

    // --- Async events (enqueued as microtasks) ---
    void notifyAction(const std::string& actionName);
    // Fired from PlatformDawn::applyConfig after a successful hot-reload.
    // No payload — listeners re-read whatever they care about via the
    // relevant `mb.*` getters (e.g. `mb.tabBarPosition`).
    void notifyConfigChanged();

    void notifyPaneCreated(TabId tab, PaneId pane);
    // nodeId is the UUID of the destroyed Terminal's tree node — passed through
    // so listeners can correlate with handles they captured from paneCreated.
    // Empty UUID is allowed (paths that don't have the UUID handy still work).
    void notifyPaneDestroyed(PaneId pane, Uuid nodeId = {});
    void notifyTabCreated(TabId tab);
    void notifyTabDestroyed(TabId tab, Uuid nodeId = {});
    // Fired before the native cleanup cascade so listeners see the live
    // pane/tab state. Exit code / signal are not plumbed yet — the v1
    // payload is {paneId, paneNodeId}.
    void notifyTerminalExited(PaneId pane, Uuid nodeId = {});
    void notifyPaneResized(PaneId pane, int cols, int rows);
    void notifyOSC(PaneId pane, int oscNum, const std::string& payload);
    void notifyForegroundProcessChanged(PaneId pane, const std::string& processName);
    void notifyPaneFocusChanged(PaneId pane, bool focused);
    void notifyFocusedPopupChanged(PaneId pane, const std::string& popupId);
    void notifyPaneMouseMove(PaneId pane, int cellX, int cellY, int pixelX, int pixelY);

    void notifyCommandComplete(PaneId pane, const CommandInfo& rec);
    // Fires when a pane's OSC 133 selected command changes (via click,
    // keyboard nav, Escape, or script API). Payload is the new command
    // id, or null when cleared.
    void notifyCommandSelectionChanged(PaneId pane, std::optional<uint64_t> commandId);

    // Deliver input to listeners on registered objects across all contexts.
    void deliverInput(const char* registryName, uint32_t key, const char* data, size_t len);
    // Deliver input to popup listeners (keyed by "paneId:popupId" string).
    void deliverPopupInput(const std::string& regKey, const char* data, size_t len);

    // Deliver input to embedded-terminal listeners (keyed by
    // "paneId:lineId" string). Mirrors deliverPopupInput.
    void deliverEmbeddedInput(const std::string& regKey, const char* data, size_t len);
    // Fire the "destroyed" event on an embedded terminal's registered
    // listeners. Called when the anchor row evicts or close() is called.
    void deliverEmbeddedDestroyed(const std::string& regKey);

    // Deliver mouse events to JS listeners.
    void deliverPopupMouseEvent(PaneId pane, const std::string& popupId,
                                 const std::string& type, int cellX, int cellY,
                                 int pixelX, int pixelY, int button);
    void deliverPaneMouseEvent(PaneId pane, const std::string& type,
                                int cellX, int cellY, int pixelX, int pixelY, int button);
    // Embedded mouse events. `type` is "press" / "release" / "move" (press
    // and release gated on the `ui` group when the applet registers the
    // listener; move is also gated on `ui`).
    void deliverEmbeddedMouseEvent(PaneId pane, uint64_t lineId,
                                    const std::string& type, int cellX, int cellY,
                                    int pixelX, int pixelY, int button);

    // Mousemove fanout. Reads `__evt_mousemove` on the registered object.
    void deliverPopupMouseMove(PaneId pane, const std::string& popupId,
                                int cellX, int cellY, int pixelX, int pixelY);
    void deliverEmbeddedMouseMove(PaneId pane, uint64_t lineId,
                                   int cellX, int cellY, int pixelX, int pixelY);

    // Resized event. Fires on popup.resize({x,y,w,h}) and embedded.resize(rows).
    // Reads `__evt_resized` on the registered object. Payload: (cols, rows).
    void deliverPopupResized(PaneId pane, const std::string& popupId, int cols, int rows);
    void deliverEmbeddedResized(PaneId pane, uint64_t lineId, int cols, int rows);

    // Run pending JS jobs. Call from main loop.
    void executePendingJobs();

    // C callback trampoline access
    const AppCallbacks& callbacks() const { return callbacks_; }
    void addPaneOutputFilter(PaneId pane, InstanceId instId);
    void addPaneInputFilter(PaneId pane, InstanceId instId);
    void addPaneMouseMoveListener(PaneId pane, InstanceId instId);

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
        struct EmbeddedRef { PaneId pane; uint64_t lineId; };
        std::vector<EmbeddedRef> ownedEmbeddeds;
        std::vector<PaneId> paneOutputFilters; // panes with output filters from this instance
        std::vector<PaneId> paneInputFilters;
        std::vector<PaneId> paneMouseMoveListeners;
    };
    Instance* findInstanceByCtx(JSContext* ctx);
    Instance* findInstance(InstanceId id);

    // Script action registration
    bool setNamespace(InstanceId id, const std::string& ns);
    bool registerAction(InstanceId id, const std::string& name);
    bool isActionRegistered(const std::string& fullName) const;
    const std::set<std::string>& registeredActions() const { return registeredActions_; }

    // Named-action handlers for JS-owned actions (NewTab, SplitPane, etc.).
    // Unlike addEventListener/notifyAction, which observe, a handler *owns* the
    // action: at most one handler per name; registration replaces the prior.
    // Native dispatch calls invokeActionHandler instead of running a built-in.
    bool registerActionHandler(InstanceId id, const std::string& name, JSValue fn);
    bool unregisterActionHandler(const std::string& name);
    // Returns true if a handler was found and called (regardless of whether
    // the handler threw). Returns false if no handler is registered — the
    // caller (Platform_Actions) logs an error in that case.
    // `buildArgs` is invoked with the handler's own JSContext so native
    // callers don't need to know which context owns the handler. It should
    // return an owning JSValue that the engine passes as the single arg.
    bool invokeActionHandler(const std::string& name,
                             const std::function<JSValue(JSContext*)>& buildArgs);

    // --- UI layout tree ---
    // Shared, window-level LayoutTree — the `mb.layout` JS surface and the
    // native Layout class both operate on this tree. Each Layout owns a
    // Container subtree inside it; those subtrees are parented under
    // `layoutRootStack()`, which represents the set of tabs (one tab = one
    // child of the Stack, activeChild = active tab).
    ::LayoutTree&       layoutTree()       { return *layoutTree_; }
    const ::LayoutTree& layoutTree() const { return *layoutTree_; }

    // Remove a subtree under `scopeRoot`. Guard: refuses if any descendant
    // Terminal is still live in the engine's terminal map (caller must kill
    // first). Cleans up paneId index entries, clears focus if it pointed at
    // the removed subtree, then detaches + destroys the nodes. Collapses any
    // singleton Container spine up to but not including `scopeRoot`. Returns
    // false on guard failure or any structural error.
    bool removeNodeSubtree(Uuid scopeRoot, Uuid nodeId);

    // Memoized root-level rects. A frame does many rect lookups — nodeRect,
    // dividersWithOwnerPanes, resizePaneEdge, resolveSubtreeContentRect,
    // tabBarRect — each of which used to run a fresh computeRects from the
    // root. The tree's dirty_ flag + cached key (fb/cell dims) short-circuit
    // repeated recomputation: we only run computeRects when the tree has
    // been mutated or the window/font metrics changed.
    const std::unordered_map<Uuid, Rect, UuidHash>&
        rootRects(uint32_t fbW, uint32_t fbH, int cellW, int cellH);

    // Root Stack holding each Tab's Layout subtree as a direct child.
    // Established in the Engine constructor and set as the tree's root.
    Uuid layoutRootStack() const { return layoutRootStack_; }

    // --- Tab identity (tree-derived) --------------------------------------
    // These read directly from the layout tree's root Stack — no separate
    // book-keeping. Each tab = one direct child of layoutRootStack_. The
    // "active tab" is the root Stack's activeChild. Order is the root
    // Stack's child order (slot order), not creation order.
    std::vector<Uuid> tabSubtreeRoots() const;
    Uuid activeTabSubtreeRoot() const;
    int  activeTabIndex() const; // -1 if no active tab / empty
    int  tabCount() const;
    // Return the tab subtreeRoot that contains `nodeId`, or nil if none.
    // Walks up nodeId's parent chain checking each against rootStack's
    // direct children.
    Uuid findTabSubtreeRootForNode(Uuid nodeId) const;

    // --- Per-tab ownership (engine-wide) ---
    // Tabs are identified by their subtreeRoot Uuid (a direct child of
    // layoutRootStack_ in the shared tree). Icon lives here, keyed by that
    // Uuid. Title lives on the tree node's `label`.
    const std::string& tabIcon(Uuid subtreeRoot) const;
    void eraseTabIcon(Uuid subtreeRoot);
    // Drop the per-tab last-focus memory when a tab is destroyed. Stale
    // entries are also lazy-ignored at read time (see rememberedFocusInSubtree),
    // so this is purely bookkeeping to bound map growth.
    void eraseLastFocusedInTab(Uuid subtreeRoot);

    // Uuid is the sole pane identity — render state, input routing, and the
    // script surface all key on it. (A previous int paneId index was purged.)

    // --- Focus + zoom (engine-wide, single active) ---
    // One Terminal is "focused" at a time (the active tab's focused pane).
    // One node is "zoomed" at a time. Both may be nil. Tab switches update
    // these through setFocusedTerminal / setZoomedNode.
    Uuid focusedTerminalNodeId() const { return focusedTerminalNodeId_; }
    // Out-of-line: also updates lastFocusedInTab_ when u resolves to a pane
    // inside a known tab subtree. Clearing focus (u = {}) deliberately does
    // not touch the memory so that transitional clears during pane removal
    // or tab close preserve the "what was focused here" answer.
    void setFocusedTerminalNodeId(Uuid u);
    // Convenience: resolve the focused Uuid through the terminal map. Null
    // if no pane is focused or the focused Uuid points at a killed Terminal
    // (the tree node may still exist briefly between killTerminal and the
    // JS-driven removeNode).
    ::Terminal* focusedTerminal() {
        return focusedTerminalNodeId_.isNil() ? nullptr : terminal(focusedTerminalNodeId_);
    }
    // NOTE: zoom state moved onto StackData::zoomTarget in the LayoutTree.
    // Engine no longer tracks a global zoom node; JS sets it directly via
    // LayoutTree::setStackZoom (bound as mb.layout.setStackZoom).

    // --- Global layout params (used to be per-Layout) ---
    int dividerPixels() const { return dividerPixels_; }
    void setDividerPixels(int px) { dividerPixels_ = px < 0 ? 0 : px; }
    // Tab-bar geometry is derived from the tree: the `TabBar` node occupies
    // its slot in the root Container; Engine::primaryTabBarNode locates the
    // first such node so renderers and input can query its rect from a
    // root-level computeRects.
    Uuid primaryTabBarNode() const;

    // Walk the layout tree from `subtreeRoot` (or root() if nil) and collect
    // every node whose kind() matches. Recurses through Container and Stack
    // children only — TabBar has no children to descend into. Order is
    // implementation-defined tree-walk order; callers needing a specific
    // ordering should sort/filter on the returned UUIDs.
    std::vector<Uuid> queryNodesByKind(NodeKind kind, Uuid subtreeRoot = {}) const;

    // Find the first node (BFS from root) whose label exactly equals
    // `label`. Returns nil if none. Empty `label` always returns nil so
    // unlabeled nodes (the default) cannot be matched by accident.
    Uuid findNodeByLabel(const std::string& label) const;

    uint32_t lastFbWidth() const { return lastFbW_; }
    uint32_t lastFbHeight() const { return lastFbH_; }
    void setLastFramebuffer(uint32_t w, uint32_t h) { lastFbW_ = w; lastFbH_ = h; }

    // Cell metrics used by the per-tab helpers (nodeRectInSubtree,
    // tabDividersWithOwnerPanes, resizeTabPaneEdge) to convert ChildSlot
    // cell-based clamps (minCells/maxCells/fixedCells) into pixels.
    // Updated by computeTabRects from its callers' font metrics. Defaults
    // to 1 so callers that never touch computeTabRects still get the old
    // pixel-equivalent behaviour.
    int lastCellW() const { return lastCellW_; }
    int lastCellH() const { return lastCellH_; }
    void setLastCellMetrics(int cellW, int cellH) {
        lastCellW_ = cellW > 0 ? cellW : 1;
        lastCellH_ = cellH > 0 ? cellH : 1;
    }

    // --- Terminal ownership (engine-wide) ---
    // The single source of truth for pane Terminal lifetime. Keyed by the
    // Terminal's tree-node UUID (same value as Terminal::nodeId()). Destroyed
    // before layoutTree_ (terminals_ is declared last in the member list).
    //
    // Defined in ScriptEngine_Terminals.cpp — a standalone TU that sees
    // Terminal.h but doesn't drag in ScriptEngine.cpp's quickjs /
    // libwebsockets dependencies. Both mb and mb-tests link it.
    ::Terminal*                 terminal(Uuid nodeId);
    const ::Terminal*           terminal(Uuid nodeId) const;
    ::Terminal*                 insertTerminal(Uuid nodeId, std::unique_ptr<::Terminal> t);
    std::unique_ptr<::Terminal> extractTerminal(Uuid nodeId);
    const std::unordered_map<Uuid, std::unique_ptr<::Terminal>, UuidHash>&
                                terminals() const { return terminals_; }

    // --- Per-tab helpers (keyed by tab subtreeRoot Uuid) -----------------
    // Every call validates that `subtreeRoot` is a direct child of
    // layoutRootStack_. Caller provides the tab Uuid from tabSubtreeRoots()
    // / activeTabSubtreeRoot() / findTabSubtreeRootForNode(). These
    // Tab operations live here; callers pass in the tab's subtreeRoot Uuid.

    // Title/icon. Title lives on the tree node's `label`; icon in tabIcons_.
    // Reads the tab's JS-set label (via mb.layout.setLabel). Pane-driven
    // title now comes from the pane's emulator title stack, pulled live by
    // the tab-bar renderer — this getter only surfaces script overrides.
    const std::string& tabTitle(Uuid subtreeRoot) const;

    // Create a fresh tab subtree (Stack + content Container) attached as an
    // orphan. Caller attaches it under layoutRootStack_.
    Uuid createTabSubtree();

    // Pane allocation + mutation (scoped to tab's subtree).
    Uuid createPaneInTab(Uuid subtreeRoot);
    Uuid allocatePaneNode(); // just layoutTree().createTerminal()
    bool splitByNodeId(Uuid existingChildNodeId, SplitDir dir,
                       Uuid newChildNodeId, bool newIsFirst);

    // Pane queries (scoped to subtreeRoot).
    ::Terminal* paneInSubtree(Uuid subtreeRoot, Uuid nodeId);
    bool hasPaneSlotInSubtree(Uuid subtreeRoot, Uuid nodeId) const;
    std::vector<::Terminal*> panesInSubtree(Uuid subtreeRoot) const;
    std::vector<::Terminal*> activePanesInSubtree(Uuid subtreeRoot) const;
    Rect nodeRectInSubtree(Uuid subtreeRoot, Uuid nodeId) const;
    Uuid paneAtPixelInSubtree(Uuid subtreeRoot, int px, int py) const;

    // Focus scoped to a tab.
    Uuid focusedPaneInSubtree(Uuid subtreeRoot) const;
    ::Terminal* focusedTerminalInSubtree(Uuid subtreeRoot);

    // Per-tab last-focused pane (maintained by setFocusedTerminalNodeId).
    // Returns the remembered pane's Uuid if still in the subtree, else nil.
    // Useful when the globally-focused pane lives in another tab but we
    // want this tab's own notion of "active pane" (e.g. tab-bar progress
    // glyph, tab-switch focus restore).
    Uuid rememberedFocusInSubtree(Uuid subtreeRoot) const;
    ::Terminal* rememberedFocusTerminalInSubtree(Uuid subtreeRoot);

    // Tab-bar rect + tab rect computation.
    Rect tabBarRect(uint32_t windowW, uint32_t windowH);
    void computeTabRects(Uuid subtreeRoot, uint32_t windowW, uint32_t windowH,
                         int cellW, int cellH);
    std::vector<Rect> tabDividerRects(Uuid subtreeRoot, int dividerPixels) const;
    std::vector<std::pair<Uuid, Rect>>
        tabDividersWithOwnerPanes(Uuid subtreeRoot, int dividerPixels) const;
    bool resizeTabPaneEdge(Uuid subtreeRoot, Uuid nodeId,
                           SplitDir axis, int pixelDelta);

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
    std::unique_ptr<::LayoutTree> layoutTree_;
    Uuid                          layoutRootStack_;
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

    struct ActionHandler {
        InstanceId id;
        JSContext* ctx;
        JSValue    fn; // owning ref; freed on replace / unregister / unload
    };
    std::unordered_map<std::string, ActionHandler> actionHandlers_;

    std::unordered_map<uint32_t, JsTimer> jsTimers_;

    std::unordered_map<PaneId, int, UuidHash> paneOutputFilterCount_;
    std::unordered_map<PaneId, int, UuidHash> paneInputFilterCount_;
    std::unordered_map<PaneId, int, UuidHash> paneMouseMoveCount_;

    JSContext* createContext();
    void setupGlobals(JSContext* ctx, InstanceId id);

    InstanceId loadScriptInternal(const std::string& path, const std::string& content,
                                   uint32_t permissions);
    void notifyPermissionRequired(const std::string& path, const std::string& permissions,
                                   const std::string& hash);

    // Fan out a mousemove event to the `__evt_mousemove` array stored on the
    // JS object registered under `registryName[key]`. Shared by popup and
    // embedded mousemove delivery paths.
    void deliverMousemoveToRegistry(const char* registryName,
                                     const std::string& key,
                                     int cellX, int cellY, int pixelX, int pixelY);

    // Fan out a resized event to the `__evt_resized` array on the object at
    // `registryName[key]`. Listeners receive `(cols, rows)`.
    void deliverResizedToRegistry(const char* registryName,
                                   const std::string& key, int cols, int rows);

    void deliverMouseToRegistry(const char* registryName, const std::string& key,
                                 const std::string& type, int cellX, int cellY,
                                 int pixelX, int pixelY, int button);

    bool runPaneFilters(PaneId pane, const char* filterProp, std::string& data);

    void cleanupPane(PaneId pane);
    void cleanupTab(TabId tab);

    // Per-tab state. All keyed by the tab's subtreeRoot Uuid (a child of
    // layoutRootStack_ in the shared tree). Declared AFTER terminals_ so
    // they destruct FIRST — overlay Terminals reference the shared tree
    // during destruction, so the tab-level containers have to go first,
    // then terminals_, then layoutTree_.
    std::unordered_map<Uuid, std::unique_ptr<::Terminal>, UuidHash> terminals_;
    std::unordered_map<Uuid, std::string,                 UuidHash> tabIcons_;
    // tab subtreeRoot → last pane Uuid that was focused inside that tab.
    // Maintained by setFocusedTerminalNodeId; cleaned up by eraseLastFocusedInTab
    // when a tab is destroyed. Lazy-validated on read against the tree.
    std::unordered_map<Uuid, Uuid,                        UuidHash> lastFocusedInTab_;

    // Engine-wide focus. Zoom lives on StackData::zoomTarget (tree-side),
    // not on the engine.
    Uuid focusedTerminalNodeId_;

    // Global layout params. Used by Tab's computeRects and divider helpers.
    int dividerPixels_ = 1;
    uint32_t lastFbW_ = 0;
    uint32_t lastFbH_ = 0;
    int lastCellW_ = 1;
    int lastCellH_ = 1;

    // --- Root-rects cache (see rootRects()) ----------------------------------
    // Populated on demand by rootRects(). Key: (fbW, fbH, cellW, cellH) and
    // the tree's revision() at compute time. Any divergence triggers a
    // recompute. Revision is monotonic (never consumed), so mutations made
    // after the per-frame dirty flag was already taken still invalidate
    // the cache correctly.
    std::unordered_map<Uuid, Rect, UuidHash> rootRectsCache_;
    uint64_t rootRectsKeyFb_       = 0; // (fbW << 32) | fbH
    int      rootRectsKeyCellW_    = 0;
    int      rootRectsKeyCellH_    = 0;
    uint64_t rootRectsKeyRevision_ = 0; // 0 = "no cache yet"
};

} // namespace Script
