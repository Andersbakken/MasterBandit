// MasterBandit scripting API — ambient TypeScript declarations.
//
// Covers:
//   - `mb` global (tabs, actions, events, script management, createSecureToken)
//   - Pane / Popup / Tab / Overlay object APIs
//   - The `mb:ws` module
//   - Timer globals (setTimeout / setInterval / clear*)
//   - Console globals
//
// NOT YET COVERED (TODO): `mb:fs`, `mb:tui`. Add those when the applet needs
// them; file lives at <repo>/types/mb.d.ts and is authoritative for any TS
// applet that transpiles down to JS before `mb.loadScript` picks it up.

// ============================================================================
// Permissions
// ============================================================================

/**
 * Comma-separated permission string passed to `mb.loadScript`. Groups expand
 * to their component bits (e.g. `"ui"` → all `ui.*` bits). See DESIGN §19.
 */
type MbPermission =
    // Groups
    | "ui" | "io" | "shell" | "actions" | "tabs" | "scripts" | "fs" | "net" | "clipboard"
    // Individual bits
    | "ui.overlay.create" | "ui.overlay.close"
    | "ui.popup.create"   | "ui.popup.destroy"
    | "io.filter.input"   | "io.filter.output" | "io.inject"
    | "shell.write"       | "shell.commands"
    | "actions.invoke"
    | "tabs.create"       | "tabs.close"
    | "scripts.load"      | "scripts.unload"
    | "fs.read"           | "fs.write"
    | "net.listen.local"
    | "clipboard.read"    | "clipboard.write"
    | "pane.selection";

/** Comma-separated permission string, e.g. `"shell,net.listen.local"`. */
type MbPermissionList = string;

// ============================================================================
// Mouse event
// ============================================================================

interface MbLinkInfo {
    readonly url: string;
    readonly startRowId: number;
    readonly startCol: number;
    readonly endRowId: number;
    /** Exclusive — one past the last column of the link. */
    readonly endCol: number;
}

interface MbMouseEvent {
    /** `"press"` | `"release"` (and possibly `"move"` for future use). */
    type: "press" | "release";
    /** Cell column (0-based). */
    cellX: number;
    /** Cell row (0-based). */
    cellY: number;
    /** Pixel x within the target. */
    pixelX: number;
    /** Pixel y within the target. */
    pixelY: number;
    /** 0=left, 1=middle, 2=right, 3=wheel-up, 4=wheel-down (xterm convention). */
    button: number;
}

// ============================================================================
// Pane
// ============================================================================

/**
 * A position within the terminal document.
 *
 * `rowId` is a stable monotonic identifier for the logical line containing
 * this position. It survives scrolling into history AND width-change reflow
 * — a soft-wrapped line keeps one `rowId` across all its physical rows.
 * `absRow` is volatile (shifts as rows scroll) and is provided for
 * convenience; prefer `rowId` for durable references.
 */
interface MbPosition {
    /** Stable logical-line ID — use with `pane.getTextFromRows()`. */
    readonly rowId: number;
    /** Volatile absolute row index at query time (archive + history + screen). */
    readonly absRow: number;
    /** Column index (0-based). */
    readonly col: number;
}

/**
 * A single command executed in this pane, populated from OSC 133 markers.
 *
 * Text fields (`command`, `output`) are extracted lazily from the document at
 * query time rather than captured eagerly — they reflect the live cell content
 * and are not subject to a size cap.
 */
interface MbCommand {
    /** Monotonic per-pane id. */
    readonly id: number;
    /** Command text echoed between `B` and `C`, whitespace-trimmed. Extracted lazily. */
    readonly command: string;
    /** Plain-text output between `C` and `D`. Extracted lazily, no size cap. */
    readonly output: string;
    /** `OSC 7` CWD value at the moment `A` fired. */
    readonly cwd: string;
    /** From `D;<exit>` or `D;err=<v>`. `null` if `D` never carried one. */
    readonly exitCode: number | null;
    /** Monotonic milliseconds when `C` fired (command started executing). */
    readonly startMs: number;
    /** Monotonic milliseconds when `D` fired (command finished). */
    readonly endMs: number;
    /** Position of the prompt marker (OSC 133;A). */
    readonly promptStart: MbPosition;
    /** Position where the command text begins (OSC 133;B). */
    readonly commandStart: MbPosition;
    /** Position where command output begins (OSC 133;C). */
    readonly outputStart: MbPosition;
    /** Position where command output ends (OSC 133;D). */
    readonly outputEnd: MbPosition;
}

interface MbPane {
    readonly id: number;
    readonly cols: number;
    readonly rows: number;
    /** OSC 2 title set by the shell. */
    readonly title: string;
    /** Working directory reported via OSC 7. */
    readonly cwd: string;
    readonly hasPty: boolean;
    readonly focused: boolean;
    /** Id of the popup that currently holds focus, or `null`. */
    readonly focusedPopupId: string | null;
    /** Foreground process name (e.g. `"zsh"`, `"vim"`). */
    readonly foregroundProcess: string;
    /** Active popups on this pane. */
    readonly popups: MbPopupInfo[];
    /**
     * Current text selection, or `null` if nothing is selected or
     * `pane.selection` permission not granted.
     * Start is always before or equal to end (normalized).
     * Column values are exclusive (one past the last selected column),
     * matching `getTextFromRows` convention. Requires `pane.selection`.
     */
    readonly selection: {
        readonly startRowId: number;
        readonly startCol: number;
        readonly endRowId: number;
        /** Exclusive — one past the last selected column. */
        readonly endCol: number;
    } | null;
    /** Current cursor position, or `null` if `pane.selection` permission not granted. Requires `pane.selection`. */
    readonly cursor: {
        readonly rowId: number;
        readonly col: number;
    } | null;
    /** Mouse position within this pane, or `null` if the mouse is outside. */
    readonly mousePosition: {
        readonly cellX: number;
        readonly cellY: number;
        readonly pixelX: number;
        readonly pixelY: number;
    } | null;
    /** Stable row ID of the oldest line in the scrollback (archive + history + screen). */
    readonly oldestRowId: number;
    /** Stable row ID of the newest (bottom-most) line. */
    readonly newestRowId: number;
    /**
     * ID of the currently highlighted OSC 133 command (set via click,
     * keyboard nav, or `selectCommand`), or `null` if none. Cleared on
     * Escape (no modifiers) and on alt-screen entry. Requires
     * `shell.commands`. Subscribe to `commandSelectionChanged` for a
     * push signal.
     */
    readonly selectedCommandId: number | null;
    /**
     * Full record of the OSC 133 command currently highlighted on this pane
     * (see `selectedCommandId`), or `null` if nothing is selected. The object
     * is the same shape as entries in `commands` — `command` and `output` are
     * lazy getters that decode on first access. Requires `shell.commands`.
     */
    readonly selectedCommand: MbCommand | null;
    /**
     * Bounded ring of recently completed commands (oldest first, most recent
     * last). Requires `shell.commands` permission.
     */
    readonly commands: readonly MbCommand[];

    /**
     * Extract plain UTF-8 text from a stable row-id range (inclusive on both
     * ends). `startRowId` maps to the first abs row of that logical line;
     * `endRowId` maps to the last abs row of that logical line — so a
     * wrapped line is covered end-to-end at the current width. IDs come from
     * `MbCommand` fields or from `rowIdAt()`. Returns an empty string if the
     * start line has been evicted from the archive. `startCol` is inclusive,
     * `endCol` is exclusive (one past the last column).
     */
    getTextFromRows(startRowId: number, startCol: number, endRowId: number, endCol: number): string;
    /**
     * Return hyperlinks (OSC 8) within a row-id range. Each entry has the URL
     * and the cell span it covers. `endCol` is exclusive. `limit` caps the
     * number of results (0 = unlimited).
     */
    getLinksFromRows(startRowId: number, endRowId: number, limit?: number): MbLinkInfo[];
    /**
     * Return the URL (OSC 8 hyperlink) at a given cell, or `null` if none.
     */
    linkAt(rowId: number, col: number): string | null;
    /**
     * Return the stable row ID for a screen row (0 = top of visible screen).
     * Returns `null` if `screenRow` is out of range (≥ terminal height).
     */
    rowIdAt(screenRow: number): number | null;
    /**
     * Set (or clear with `null`) the OSC 133 command selection highlight.
     * `id` must refer to a live command in `pane.commands`; unknown ids are
     * silently treated as `null`. Requires `shell.commands`.
     */
    selectCommand(id: number | null): void;

    /** Emit data into the terminal emulator (as if the PTY wrote it). Requires `io.inject`. */
    inject(data: string): void;
    /** Write data to the PTY master (shell stdin). Requires `shell.write`. Throws if no PTY. */
    write(data: string): void;
    /** Create a popup on this pane. Requires `ui.popup.create`. Returns null on failure. */
    createPopup(opts: { id: string; x: number; y: number; w: number; h: number }): MbPopup | null;

    /** Synchronous input filter — return a replacement string, or void to pass through. Requires `io.filter.input`. */
    addEventListener(event: "input",  fn: (data: string) => string | void): void;
    /** Synchronous output filter — return a replacement string, or void to pass through. Requires `io.filter.output`. */
    addEventListener(event: "output", fn: (data: string) => string | void): void;
    /** Mouse events on the pane. Requires `ui`. */
    addEventListener(event: "mouse",  fn: (ev: MbMouseEvent) => void): void;
    /** Called when the pane is resized. */
    addEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    /** Called once when the pane is destroyed (shell exit or pane close). */
    addEventListener(event: "destroyed", fn: () => void): void;
    /** Fired when the pane's foreground process changes (e.g. shell → vim). */
    addEventListener(event: "foregroundProcessChanged", fn: (processName: string) => void): void;
    /** Fired on mouse movement within the pane. Requires `ui`. */
    addEventListener(event: "mousemove", fn: (ev: { cellX: number; cellY: number; pixelX: number; pixelY: number }) => void): void;
    /** Fired when the pane gains or loses focus. */
    addEventListener(event: "focusChanged", fn: (focused: boolean) => void): void;
    /** Fired when the focused popup changes. `popupId` is `null` when no popup is focused. */
    addEventListener(event: "focusedPopupChanged", fn: (popupId: string | null) => void): void;
    /**
     * Fired when a handler-less OSC with the given number arrives. Payload is
     * the raw OSC body with the leading `N;` stripped. The event name is
     * `"osc:58237"`, `"osc:9"`, etc.
     */
    addEventListener(event: `osc:${number}`, fn: (payload: string) => void): void;
    /**
     * Fires once per completed shell command (OSC 133;D arrival). The callback
     * receives an `MbCommand` record with exit code, timing, cwd, line IDs, and
     * lazily-extracted `command`/`output` text. Requires `shell.commands` permission.
     */
    addEventListener(event: "commandComplete", fn: (cmd: MbCommand) => void): void;
    /**
     * Fires when the pane's OSC 133 command selection changes (click,
     * keyboard nav, Escape, script `selectCommand`, or alt-screen entry).
     * Payload is the new selected command id or `null` when cleared.
     */
    addEventListener(event: "commandSelectionChanged", fn: (commandId: number | null) => void): void;

    removeEventListener(event: "input",  fn: (data: string) => string | void): void;
    removeEventListener(event: "output", fn: (data: string) => string | void): void;
    removeEventListener(event: "mouse",  fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
    removeEventListener(event: "foregroundProcessChanged", fn: (processName: string) => void): void;
    removeEventListener(event: "mousemove", fn: (ev: { cellX: number; cellY: number; pixelX: number; pixelY: number }) => void): void;
    removeEventListener(event: "focusChanged", fn: (focused: boolean) => void): void;
    removeEventListener(event: "focusedPopupChanged", fn: (popupId: string | null) => void): void;
    removeEventListener(event: `osc:${number}`, fn: (payload: string) => void): void;
    removeEventListener(event: "commandComplete", fn: (cmd: MbCommand) => void): void;
    removeEventListener(event: "commandSelectionChanged", fn: (commandId: number | null) => void): void;
}

interface MbPopupInfo {
    id: string;
    x: number;
    y: number;
    w: number;
    h: number;
    focused: boolean;
}

// ============================================================================
// Popup
// ============================================================================

interface MbPopup {
    readonly paneId: number;
    readonly id: string;
    readonly focused: boolean;
    readonly cols: number;
    readonly rows: number;
    readonly x: number;
    readonly y: number;

    /** Render data into the popup (treated like PTY output). */
    inject(data: string): void;
    /** Resize/move the popup. */
    resize(opts: { x: number; y: number; w: number; h: number }): void;
    /** Close and destroy the popup. */
    close(): void;

    /** Keyboard events when the popup has focus. Requires `io.filter.input`. */
    addEventListener(event: "input", fn: (data: string) => void): void;
    /** Mouse events on the popup. Requires `ui`. */
    addEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    /** Fired once when the popup is closed. */
    addEventListener(event: "destroyed", fn: () => void): void;

    removeEventListener(event: "input", fn: (data: string) => void): void;
    removeEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
}

// ============================================================================
// Tab
// ============================================================================

interface MbTab {
    readonly id: number;
    readonly panes: MbPane[];
    readonly activePane: MbPane | undefined;
    readonly overlay: MbOverlay | undefined;

    /** Create a headless overlay on this tab. Requires `ui.overlay.create`. */
    createOverlay(): MbOverlay | null;
    /** Close the tab's active overlay, if any. */
    closeOverlay(): void;
    /** Close this tab. Requires `tabs.close`. */
    close(): void;

    addEventListener(event: "destroyed", fn: () => void): void;
    addEventListener(event: "overlayCreated", fn: (overlay: MbOverlay) => void): void;
    addEventListener(event: "overlayDestroyed", fn: () => void): void;

    removeEventListener(event: "destroyed", fn: () => void): void;
    removeEventListener(event: "overlayCreated", fn: (overlay: MbOverlay) => void): void;
    removeEventListener(event: "overlayDestroyed", fn: () => void): void;
}

// ============================================================================
// Overlay
// ============================================================================

interface MbOverlay {
    readonly cols: number;
    readonly rows: number;
    readonly hasPty: boolean;

    /** Emit data into the overlay's terminal. */
    inject(data: string): void;
    /** Write to the overlay's PTY if it has one. */
    write(data: string): void;
    /** Close this overlay. */
    close(): void;

    /** Extract plain text from a stable row-id range. `startCol` inclusive, `endCol` exclusive. */
    getTextFromRows(startRowId: number, startCol: number, endRowId: number, endCol: number): string;
    /** Stable row ID for a screen row (0 = top). Returns null if out of range. */
    rowIdAt(screenRow: number): number | null;

    /** Keyboard events when the overlay is focused. Requires `io.filter.input`. */
    addEventListener(event: "input", fn: (data: string) => void): void;
    addEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    addEventListener(event: "destroyed", fn: () => void): void;

    removeEventListener(event: "input", fn: (data: string) => void): void;
    removeEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
}

// ============================================================================
// Script loading result
// ============================================================================

/**
 * Outcome of `mb.loadScript` / `mb.approveScript`. Narrow via `status`:
 * the `id` field is only present on `"loaded"`, and `error` only on `"error"`.
 */
type MbLoadResult =
    | { status: "loaded"; id: number }
    | { status: "pending" }
    | { status: "denied" }
    | { status: "error"; error?: string };

// ============================================================================
// Action registry
// ============================================================================

interface MbActionInfo {
    name: string;
    label: string;
    /** True for C++-defined actions, false for script-registered ones. */
    builtin: boolean;
    /** Optional args the action accepts (e.g. `["right","left"]` for directional). */
    args?: string[];
}

// ============================================================================
// mb global
// ============================================================================

interface MbGlobal {
    /** All open tabs. */
    readonly tabs: MbTab[];
    /** The currently focused pane, or undefined if none. */
    readonly activePane: MbPane | undefined;
    /** The currently active tab, or undefined if none. */
    readonly activeTab: MbTab | undefined;
    /** All available actions (built-in + script-registered). */
    readonly actions: MbActionInfo[];

    // --- Actions ---
    invokeAction(name: string, ...args: string[]): boolean;
    /** Set this script's action namespace. Can only be called once per instance. */
    setNamespace(namespace: string): boolean;
    /** Register `<namespace>.<name>` as a script action. */
    registerAction(name: string): boolean;

    // --- Tabs ---
    /** Requires `tabs.create`. Returns the new tab's id. */
    createTab(): number;
    /** Requires `tabs.close`. Closes the given tab (or the active one if omitted). */
    closeTab(id?: number): void;

    // --- Script management ---
    /**
     * Requires `scripts.load`. Returns an outcome object:
     *  - `{ status: "loaded", id }`      — script is running, `id` is the instance id.
     *  - `{ status: "pending" }`         — allowlist miss, permission prompt raised;
     *                                      a `scriptPermissionRequired` event has been
     *                                      queued and the final outcome will be the
     *                                      return value of a matching `approveScript`.
     *  - `{ status: "denied" }`          — permanently denied per allowlist.
     *  - `{ status: "error", error }`    — file unreadable or JS evaluation failed.
     */
    loadScript(path: string, permissions?: MbPermissionList): MbLoadResult;
    /** Requires `scripts.unload`. */
    unloadScript(id: number): void;
    /**
     * Respond to a `scriptPermissionRequired` event. Built-in scripts only.
     * Returns the final outcome, same shape as `loadScript` (minus `pending`):
     * `loaded` for y/a if the subsequent eval succeeded, `error` if it failed,
     * `denied` for n/d.
     */
    approveScript(path: string, response: "y" | "n" | "a" | "d"): MbLoadResult;
    /** Schedule self-unload via a zero-delay timer. */
    exit(): void;

    // --- Tokens / crypto ---
    /**
     * Generate `length` cryptographically-secure random bytes and return them
     * as a `2 * length`-character hex string. Defaults to 32 bytes (64 hex
     * chars). Ungated — no permission required.
     */
    createSecureToken(length?: number): string;

    // --- Custom terminal capabilities ---
    /** Register a custom XTGETTCAP capability. */
    registerTcap(name: string, value: string): void;
    /** Remove a custom XTGETTCAP capability. */
    unregisterTcap(name: string): void;

    // --- Clipboard ---
    /**
     * Read from the system clipboard. Requires `clipboard.read`.
     * @param source `"clipboard"` (default) or `"primary"` (X11 primary selection).
     */
    getClipboard(source?: "clipboard" | "primary"): string;
    /**
     * Write to the system clipboard. Requires `clipboard.write`.
     * @param source `"clipboard"` (default) or `"primary"` (X11 primary selection).
     */
    setClipboard(text: string, source?: "clipboard" | "primary"): void;

    // --- Lifecycle events ---
    /** Fires once per new pane. Fires on every loaded instance. */
    addEventListener(event: "paneCreated", fn: (pane: MbPane) => void): void;
    /** Fires once per new tab. */
    addEventListener(event: "tabCreated", fn: (tab: MbTab) => void): void;
    /** Fires when any action is invoked, with the action's full name. */
    addEventListener(event: "action", fn: (actionName: string) => void): void;
    /**
     * Fires on the specific action name. Built-in scripts receive every action;
     * user scripts only receive actions in their own namespace.
     */
    addEventListener(event: "action", actionName: string, fn: (...args: string[]) => void): void;
    /**
     * Fired when a user script is trying to load and the allowlist lacks a
     * matching entry. Built-in scripts only. Respond via `mb.approveScript`.
     */
    addEventListener(
        event: "scriptPermissionRequired",
        fn: (path: string, permissions: string, hash: string) => void
    ): void;

    removeEventListener(event: "paneCreated", fn: (pane: MbPane) => void): void;
    removeEventListener(event: "tabCreated", fn: (tab: MbTab) => void): void;
    removeEventListener(event: "action", fn: (actionName: string) => void): void;
    removeEventListener(event: "action", actionName: string, fn: (...args: string[]) => void): void;
    removeEventListener(
        event: "scriptPermissionRequired",
        fn: (path: string, permissions: string, hash: string) => void
    ): void;
}

// ============================================================================
// Globals
// ============================================================================

declare const mb: MbGlobal;

// Note: `console`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`
// and the ES built-in types (`ArrayBuffer`, `Uint8Array`, etc.) are available
// at runtime in MB's QuickJS-ng engine but are NOT declared here — they
// conflict with `lib.dom.d.ts` if included. Cover them in your project's
// tsconfig `lib` (e.g. `"es2022"` + your own minimal timer/console shim, or
// `"dom"` if you're OK with the browser-flavoured signatures).

// ============================================================================
// mb:ws — WebSocket server module
// ============================================================================

declare module "mb:ws" {
    /**
     * Options for `createServer`. Currently loopback-only: `host` must be
     * `"127.0.0.1"` or `"localhost"`.
     */
    export interface MbWsServerOptions {
        host: "127.0.0.1" | "localhost";
        /** 0 = OS picks a free port (read back via `server.port`). */
        port: number;
        /**
         * Token that clients must present in `Sec-WebSocket-Protocol` as
         * `mb-shell.<token>`. Mismatched clients are rejected at handshake.
         * Generate via `mb.createSecureToken()`.
         */
        token: string;
    }

    export interface MbWsConnection {
        /**
         * Send a frame. Strings are sent as text frames; `ArrayBuffer` /
         * `Uint8Array` are sent as binary frames.
         */
        send(data: string | ArrayBuffer | Uint8Array): void;
        /** Initiate graceful close. The `close` event fires when complete. */
        close(): void;

        /** Text frame received as string; binary frame received as `ArrayBuffer`. */
        addEventListener(event: "message", fn: (data: string | ArrayBuffer) => void): void;
        /** Fired once when the connection ends for any reason. */
        addEventListener(event: "close", fn: () => void): void;
    }

    export interface MbWsServer {
        /** Bound TCP port (populated after createServer returns). */
        readonly port: number;
        /** Close the server and all its connections. Idempotent. */
        close(): void;

        /** Fires for every successful client handshake. */
        addEventListener(event: "connection", fn: (conn: MbWsConnection) => void): void;
        /** Fires on bind errors and similar server-level problems. */
        addEventListener(event: "error", fn: (err: Error) => void): void;
    }

    /**
     * Create a WebSocket server. Requires `net.listen.local` permission.
     * Throws if binding fails.
     */
    export function createServer(opts: MbWsServerOptions): MbWsServer;

    const _default: {
        createServer: typeof createServer;
    };
    export default _default;
}
