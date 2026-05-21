// MasterBandit scripting API — ambient TypeScript declarations.
//
// Covers:
//   - `mb` global (tabs, actions, events, script management, createSecureToken,
//     createUuid, quit, clipboard, tcap, config (read + mutators), lifecycle events)
//   - `mb.layout` — LayoutTree primitives (containers, stacks, tab bars,
//     tabs, terminal spawn, slot constraints, queries)
//   - Terminal base + Pane / Popup / EmbeddedTerminal / Tab APIs
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
    | "ui" | "io" | "shell" | "actions" | "tabs" | "scripts" | "fs" | "net" | "clipboard" | "layout"
    | "process"
    // Individual bits
    | "ui.popup.create"   | "ui.popup.destroy" | "ui.focus"
    | "io.filter.input"   | "io.filter.output" | "io.inject"
    | "shell.write"       | "shell.commands"
    | "actions.invoke"
    | "tabs.create"       | "tabs.close"
    | "scripts.load"      | "scripts.unload"
    | "fs.read"           | "fs.write"
    | "net.listen.local"
    | "clipboard.read"    | "clipboard.write"
    /**
     * Read pane document content: `selection`, `cursor`, scrollback text
     * extraction (`getTextFromRows` / `getLinksFromRows` / `linkAt` /
     * `rowIdAt` / `writeRangeToFile`) and the `rowEvicted` lifecycle
     * event.
     */
    | "pane.read"
    | "layout.modify"
    /** Launch external (non-PTY) processes via `mb.process.spawn`. */
    | "process.spawn"
    /**
     * Every regular permission bit (all groups + standalone bits). The
     * loaded instance still runs sandboxed. Does NOT include `"builtin"`
     * — for full trust use that instead.
     */
    | "all"
    /**
     * Elevates the script to built-in trust: every permission bit and no
     * sandbox restrictions (unrestricted file paths, no allowlist prompt).
     *
     * **Caller restriction**: only built-in scripts may request this. A
     * user script that names it triggers a permission violation and is
     * terminated.
     *
     * Implies `"all"`.
     */
    | "builtin";

/** Comma-separated permission string, e.g. `"shell,net.listen.local"`. */
type MbPermissionList = string;

// ============================================================================
// Process spawn
// ============================================================================

/**
 * Options bag for `mb.process.spawn`.
 *
 * `cwd` and `env` are independently optional — pass either, both, or
 * neither. Omitted means "inherit from mb's process".
 */
interface MbProcessSpawnOptions {
    /**
     * Working directory for the spawned process. Empty string or omitted
     * → inherit mb's cwd. Post-fork chdir failure aborts the spawn
     * silently (no synchronous error channel).
     */
    cwd?: string;
    /**
     * Environment variable overrides — each key replaces (or appends) the
     * corresponding entry in the inherited environment. Keys absent here
     * keep their inherited value. No syntax for *removing* an inherited
     * key (workaround: set to empty string). Non-string values are
     * silently dropped.
     */
    env?: { [key: string]: string };
}

interface MbProcess {
    /**
     * Launch an external process detached from mb. Fire-and-forget: the
     * spawned process runs as its own session leader, stdio is redirected
     * to `/dev/null`, and the child is reaped by init/launchd. No
     * exit-code or output-capture surface — use `mb.layout.createTerminal`
     * if you need to read output or wait on the process.
     *
     * `path` is resolved via PATH (`execvp(3)`). No shell — args pass
     * verbatim with no metacharacter expansion. argv[0] is set to `path`
     * automatically; `args` becomes argv[1..N].
     *
     * Returns the intermediate-child pid on successful fork, `0` on
     * pre-fork failure. This is NOT the grandchild pid running the binary
     * (the double-fork detaches that).
     *
     * Does NOT throw on grandchild exec failure (e.g. binary missing) —
     * failure is logged but the script sees a pid return. Validate with
     * `fs.statSync` first if you need exec confirmation.
     *
     * @throws TypeError on missing/invalid args or missing `process.spawn`
     *   permission.
     *
     * @example
     *   mb.process.spawn(process.env.EDITOR ?? "vi", ["/etc/hosts"]);
     *   mb.process.spawn("eslint", ["--fix", "src/"], {
     *     cwd: "/home/me/project",
     *     env: { NODE_OPTIONS: "--max-old-space-size=4096" }
     *   });
     */
    spawn(path: string, args?: string[], opts?: MbProcessSpawnOptions): number;
}

// ============================================================================
// Output capture
// ============================================================================

/**
 * Encoding for captured PTY output.
 *
 * - `"raw"`: every byte the emulator received, verbatim — ANSI/CSI/OSC
 *   escapes included. Replay with `less -R` or `cat`. Smallest format.
 * - `"text"`: escape-stripped plain text. ANSI/CSI/OSC/DCS/SS3 and most C0
 *   controls dropped (keeps `\n` and `\t`). `\r\n` → `\n`, bare `\r`
 *   dropped. UTF-8 passes through. In-place updates (progress bars) leave
 *   duplicated content — no cursor-driven overwrite modelling.
 * - `"asciicast"`: asciicast v2 (https://docs.asciinema.org). Header line +
 *   per-chunk `[<elapsed>, "o", <data>]` JSON-lines records. Replayable
 *   with the asciinema toolchain.
 */
type MbOutputCaptureFormat = "raw" | "text" | "asciicast";

/** Options for `pane.captureOutputToFile`. */
interface MbOutputCaptureOptions {
    /** On-disk format. Defaults to `"raw"`. */
    format?: MbOutputCaptureFormat;
}

/**
 * Reason a capture stopped, surfaced via the `stopped` event.
 *
 * - `"explicit"`: `.stop()` was called, or the owning script was unloaded.
 *   `error` is the empty string.
 * - `"io-error"`: a write to the destination file failed (disk full, EIO,
 *   etc.). Capture has been auto-stopped; `error` carries the strerror.
 */
type MbOutputCaptureStopReason = "explicit" | "io-error";

/** Payload of the `stopped` event on `MbOutputCapture`. */
interface MbOutputCaptureStoppedEvent {
    reason: MbOutputCaptureStopReason;
    /** strerror text on `io-error`; empty string otherwise. */
    error: string;
}

/**
 * Handle returned by `pane.captureOutputToFile`. Methods become no-ops
 * after stop; `active` is the canonical "still capturing?" flag.
 *
 * Multiple captures per pane are supported, but each must point at a
 * distinct path. Calling `pane.captureOutputToFile` twice with the same
 * path on the same pane throws.
 */
interface MbOutputCapture {
    /** Sandbox-validated absolute path being written to. */
    readonly path: string;
    /** Format passed at registration time. */
    readonly format: MbOutputCaptureFormat;
    /**
     * `true` until the capture stops. Flips to `false` BEFORE the
     * `stopped` event fires, so handlers reading `active` in the callback
     * see the post-stop state.
     */
    readonly active: boolean;
    /** UUID string of the pane this capture is attached to. */
    readonly paneId: string;

    /**
     * Stop this capture and fire `stopped` with reason `"explicit"`.
     * Returns `true` if it closed a live capture, `false` if already
     * stopped (idempotent).
     */
    stop(): boolean;

    /**
     * Subscribe to capture lifecycle. Only `"stopped"` is defined; other
     * names install listeners that never fire.
     */
    addEventListener(
        event: "stopped",
        fn: (ev: MbOutputCaptureStoppedEvent) => void
    ): void;
    removeEventListener(
        event: "stopped",
        fn: (ev: MbOutputCaptureStoppedEvent) => void
    ): void;
}

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

/**
 * One match returned from `pane.findText`. Anchored on logical-line ids.
 * `startCol` / `endCol` are cumulative cell offsets within the logical
 * line (NOT visual-row columns); directly usable as `addDecoration`
 * offsets. `endCol` is exclusive. Each match is confined to a single
 * logical line (`startRowId === endRowId`).
 */
interface MbMatch {
    readonly startRowId: number;
    readonly startCol: number;
    readonly endRowId: number;
    /** Exclusive — one past the last column of the match. */
    readonly endCol: number;
}

interface MbMouseEvent {
    /**
     * `"press"` / `"release"` for button events, `"move"` for cursor
     * motion (only delivered to subscribers that opted in via the `ui`
     * permission), `"wheel"` for scroll-wheel notches.
     */
    type: "press" | "release" | "move" | "wheel";
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
    /**
     * Wheel delta in scroll-line units. Non-zero only for `type ==
     * "wheel"`; positive = scroll content up (toward older scrollback),
     * negative = scroll content down. Magnitude is platform-supplied
     * (commonly ±1 per notch).
     */
    delta: number;
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

/**
 * Common base for everything backed by a live terminal emulator: `MbPane`,
 * `MbPopup`, `MbEmbeddedTerminal`. The global `Terminal` is this class —
 * `x instanceof Terminal` is true for every subclass instance.
 */
interface MbTerminal {
    /** Viewport width in character cells. */
    readonly cols: number;
    /** Viewport height in character cells. */
    readonly rows: number;
    /**
     * Current cursor state. `rowId` is a stable logical-line id (same
     * numbering as `pane.oldestRowId`/`newestRowId`). On a pane requires
     * `pane.read`; on popups/embeddeds it's ungated.
     */
    readonly cursor: {
        readonly rowId: number;
        readonly col: number;
        readonly visible: boolean;
    } | null;
    /** Discriminator for the concrete subclass. */
    readonly kind: "pane" | "popup" | "embedded";
    /** Pixel width of one cell at the current font / DPI. Window-global. */
    readonly cellWidth: number;
    /** Pixel height of one cell at the current font / DPI. Window-global. */
    readonly cellHeight: number;

    /** Emit data into the terminal emulator (as if the PTY wrote it). Requires `io.inject`. */
    inject(data: string): void;

    /**
     * Add a presentation overlay anchored to logical-line ids. Survives
     * scroll, autowrap reflow, and scrollback eviction until the anchor
     * line evicts past the archive cap. Returns a handle usable with
     * `removeDecoration` or `handle.remove()`. Requires `pane.read`.
     *
     * Composition: User decorations paint on top of cell SGR, below
     * hyperlink underlines, the OSC 133 command region, and selection.
     * fg / bg / strikethrough are last-writer-wins by insertion order
     * (broken by `zPriority`). Underline is first-writer-wins and never
     * overrides a cell's own SGR underline.
     *
     * `tag` groups decorations for `clearDecorations(tag)`. The reserved
     * tags `"selection"`, `"command-region"`, `"hyperlink"` are rejected.
     */
    addDecoration(spec: {
        startRowId: number;
        /** Cell-offset within the logical line. Inclusive. */
        startCol: number;
        endRowId: number;
        /**
         * Cell-offset within the logical line. **Exclusive** — one past the
         * last covered cell, matching `pane.selection.endCol`,
         * `getTextFromRows`, `MbCommand.outputEnd.col`, `MbLinkInfo.endCol`,
         * and `MbMatch.endCol`. A single-line decoration with
         * `endCol <= startCol` is rejected (paints nothing).
         */
        endCol: number;
        style?: {
            /** Packed RGBA8 (0xAABBGGRR — alpha in MSB). */
            fg?: number;
            /** Packed RGBA8 (0xAABBGGRR — alpha in MSB). */
            bg?: number;
            underline?: {
                /** 0=straight, 1=double, 2=curly, 3=dotted */
                style: number;
                /** Packed RGBA8; omit / 0 = use cell fg. */
                color?: number;
            };
            strikethrough?: boolean;
            /** Per-side bg rect expansion in pixels. Default 0. */
            bgInflateX?: number;
            bgInflateY?: number;
        };
        tag?: string;
        zPriority?: number;
        shape?: "range" | "rectangle";
    }): MbDecorationHandle;

    /**
     * Remove a decoration by its numeric id. Prefer `handle.remove()`.
     * Returns true iff a decoration was actually removed.
     */
    removeDecoration(id: number): boolean;

    /**
     * Remove User decorations. With no arg or empty tag, clears every
     * User decoration; system kinds (selection / command-region /
     * hyperlink) are never touched. Returns the count cleared.
     */
    clearDecorations(tag?: string): number;

    /**
     * Begin a decoration batch. Queues `addDecoration`/`clearDecorations`
     * calls without mutating the terminal; `submit()` applies them
     * atomically with one snapshot publish. Use to avoid mid-burst
     * flicker when scripts emit many decoration mutations in a row.
     */
    createDecorationBatch(): MbDecorationBatch;
}

/**
 * Atomic decoration mutation. Created by `Terminal.createDecorationBatch()`.
 * `addDecoration` and `clearDecorations` queue ops; they DO NOT mutate the
 * terminal. `submit()` applies all queued ops in order under a single lock
 * acquisition with at most one snapshot publish at the end.
 *
 * Calling any method after `submit()` throws.
 */
interface MbDecorationBatch {
    /** Queue an Add op. Spec is the same as `Terminal.addDecoration`. Returns the batch (chainable). */
    addDecoration(spec: Parameters<MbTerminal["addDecoration"]>[0]): MbDecorationBatch;

    /** Queue a Clear op. Tag semantics match `Terminal.clearDecorations`. Returns the batch. */
    clearDecorations(tag?: string): MbDecorationBatch;

    /** Apply all queued ops atomically. Returns DecorationHandles for Add ops in queue order. */
    submit(): MbDecorationHandle[];
}

/**
 * Animatable decoration properties. Colors are packed RGBA8 (0xAABBGGRR);
 * inflate values are pixel ints. `underlineColor` only takes visible
 * effect when the decoration has `style.underline` set.
 */
type MbDecorationProp = "bg" | "fg" | "underlineColor" | "bgInflateX" | "bgInflateY";

/** Built-in easing curves for `mb.startAnimation`. */
type MbEase = "linear" | "easeIn" | "easeOut" | "easeInOut" | "easeInOutCubic";

/**
 * Parameters for an "ease" animation (the only animation type in v1).
 * `startValue` defaults to the property's currently sampled value (clean
 * handoff from a prior animation).
 */
interface MbEaseAnimParams {
    type?: "ease";
    startValue?: number;
    endValue: number;
    durationMs: number;
    ease?: MbEase;
}

/**
 * Handle to a registered decoration. Returned by `addDecoration` and
 * `batch.submit()`.
 */
interface MbDecorationHandle {
    readonly id: number;
    /** Remove this decoration. Active animations are auto-cancelled. */
    remove(): boolean;
    /** Equivalent to `mb.startAnimation(this, prop, params)`. */
    animate(prop: MbDecorationProp, params: MbEaseAnimParams): MbAnimationHandle;
}

/**
 * Handle to a running or settled animation. Animations are fire-and-
 * forget — the handle can be discarded. Hold it only to cancel
 * mid-flight or await completion.
 */
interface MbAnimationHandle {
    /**
     * Cancel. `"stop"` (default) freezes at the current value;
     * `"snap-to-end"` jumps to `endValue` and resolves `onEnd` as
     * `"completed"`. No-op if already settled.
     */
    cancel(mode?: "stop" | "snap-to-end"): void;
    /**
     * Promise resolving when the animation finishes. `"completed"` for
     * natural completion or `cancel("snap-to-end")`; `"cancelled"` for
     * `cancel()` or target destruction.
     */
    onEnd(): Promise<"completed" | "cancelled">;
}

declare const Terminal: MbTerminal;

interface MbPane extends MbTerminal {
    /**
     * Stable UUID of this pane's Terminal node in `mb.layout`. Use with
     * `mb.layout.splitPane`, `focusPane`, `removeNode`, `killTerminal`,
     * `node`. Ungated — `mb.layout` mutations carry their own permissions.
     */
    readonly nodeId: string;
    /**
     * Effective pane title. Reads return the script-supplied override
     * if one is set (via assignment), otherwise the OSC 0/2 title the
     * shell wrote, otherwise `""`.
     *
     * Assigning a string sets a custom override that takes precedence
     * over OSC titles — empty string is a valid (empty) custom title.
     * Assigning `null` or `undefined` clears the override; the next
     * read returns whatever OSC title is currently active.
     *
     * The override is window-local state attached to the pane; it is
     * not persisted and disappears with the pane. Subscribe to
     * `"titleChanged"` for a push signal whenever the effective title
     * changes (shell-driven or script-driven).
     */
    title: string | null;
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
     * Current text selection, normalized (start ≤ end), with exclusive
     * `endCol`. `null` if nothing selected. Requires `pane.read`.
     */
    readonly selection: {
        readonly startRowId: number;
        readonly startCol: number;
        readonly endRowId: number;
        /** Exclusive — one past the last selected column. */
        readonly endCol: number;
    } | null;
    // `cursor` (inherited from MbTerminal) requires `pane.read` on a pane.

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
     * ID of the currently highlighted OSC 133 command, or `null` if none.
     * Cleared on Escape (no modifiers) and on alt-screen entry. Subscribe
     * to `commandSelectionChanged` for a push signal. Requires
     * `shell.commands`.
     */
    readonly selectedCommandId: number | null;
    /**
     * Full record of the highlighted OSC 133 command, or `null` if none.
     * Same shape as `commands` entries. Requires `shell.commands`.
     */
    readonly selectedCommand: MbCommand | null;
    /**
     * Bounded ring of recently completed commands (oldest first). Requires
     * `shell.commands`.
     */
    readonly commands: readonly MbCommand[];
    /**
     * `true` while the pane's emulator is on the alternate screen (DECSET
     * 1049 active — vim, less, htop, etc.). Subscribe to `altScreenChanged`
     * for a push signal. Scrollback APIs (`findText`, `getTextFromRows`,
     * `getLinksFromRows`, `linkAt`) always operate on the main-screen
     * document and ignore alt-screen contents; gate those calls on this
     * flag if your script's UX depends on what the user actually sees.
     */
    readonly usingAltScreen: boolean;

    /**
     * Extract plain UTF-8 text from a stable row-id range (inclusive on
     * both ends). `startRowId`/`endRowId` map to the first/last abs row of
     * the logical line — a wrapped line is covered end-to-end at the
     * current width. `startCol` inclusive, `endCol` exclusive. Returns
     * empty string if the start line has been evicted past the archive
     * cap. Requires `pane.read`.
     */
    getTextFromRows(startRowId: number, startCol: number, endRowId: number, endCol: number): string;
    /**
     * Write the same text `getTextFromRows` would return directly to
     * `path`. Atomic on success (writes to `<path>.tmp` and renames).
     * Auto-creates parent directories. Path is sandboxed as
     * `fs.writeFileSync` (built-ins unrestricted; user scripts confined
     * to `<configDir>/<scriptStem>/`). Empty ranges write a zero-byte
     * file. Returns bytes written. Requires `pane.read` + `fs.write`.
     */
    writeRangeToFile(path: string, startRowId: number, startCol: number, endRowId: number, endCol: number): number;
    /**
     * Open a streaming capture of this pane's PTY output. Returns a
     * handle the script can later `.stop()`. Multiple concurrent captures
     * per pane are supported as long as their paths are distinct.
     *
     * Captured bytes are post-coalesce / pre-filter — they reflect what
     * the emulator saw on the wire, regardless of any script-level output
     * filter attached.
     *
     * Path is sandboxed as `fs.writeFileSync` (built-ins unrestricted;
     * user scripts confined to `<configDir>/<scriptStem>/` or
     * `/tmp/masterbandit/`). Parent directories auto-created; existing
     * files truncated.
     *
     * On a write failure (disk full, EIO, ...), the capture auto-stops
     * and the handle's `stopped` event fires with `reason="io-error"`.
     * Other captures on the same pane are unaffected.
     *
     * Captures are tracked on the calling instance — unloading the
     * script auto-stops every capture it owns.
     *
     * Requires `pane.read` + `fs.write`. Throws on permission denial,
     * sandbox violation, duplicate path on the same pane, or open
     * failure.
     *
     * @example
     *   // Tail the user's shell into a JSON-Lines asciicast for an
     *   // LLM helper to follow along with.
     *   const cap = pane.captureOutputToFile(
     *     "/tmp/masterbandit/session.cast",
     *     { format: "asciicast" }
     *   );
     *   cap.addEventListener("stopped", ev => {
     *     if (ev.reason === "io-error") console.error("capture died:", ev.error);
     *   });
     *   // ... later
     *   cap.stop();
     */
    captureOutputToFile(path: string, opts?: MbOutputCaptureOptions): MbOutputCapture;
    /**
     * Return hyperlinks (OSC 8) within a row-id range. Each entry has the URL
     * and the cell span it covers. `endCol` is exclusive. `limit` caps the
     * number of results (0 = unlimited). Requires `pane.read`.
     */
    getLinksFromRows(startRowId: number, endRowId: number, limit?: number): MbLinkInfo[];
    /**
     * Return the URL (OSC 8 hyperlink) at a given cell, or `null` if none.
     * Requires `pane.read`.
     */
    linkAt(rowId: number, col: number): string | null;
    /**
     * Return the stable row ID for a screen row (0 = top of visible screen).
     * Returns `null` if `screenRow` is out of range (≥ terminal height).
     * Requires `pane.read`.
     */
    rowIdAt(screenRow: number): number | null;
    /**
     * Search the pane's scrollback + visible grid for `needle`. Matches
     * are confined to a single logical line each; cell columns are
     * cumulative within the logical line, usable as `addDecoration`
     * offsets.
     *
     * Default: case-insensitive literal substring. `opts.regex` switches
     * to ECMAScript regex (invalid syntax → `[]`, not a throw).
     * `opts.caseSensitive` disables folding. `opts.wholeWord` requires
     * word boundaries (literal mode only — regex callers use `\b`).
     * `opts.limit` caps results.
     *
     * Walks oldest → newest. Empty `needle` returns `[]`. Lines
     * straddling the scrollback/visible-grid boundary are searched on the
     * scrollback side only.
     *
     * Requires `pane.read`.
     */
    findText(needle: string, opts?: {
        /** Treat `needle` as ECMAScript regex. Default `false`. */
        regex?: boolean;
        /** Default `false` (i.e. case-insensitive). */
        caseSensitive?: boolean;
        /** Match only at word boundaries. Literal-mode only. Default `false`. */
        wholeWord?: boolean;
        /** Hard cap; <= 0 means no cap. Default 10000. */
        limit?: number;
    }): MbMatch[];
    /**
     * Scroll so the line with `rowId` is at the top. Returns `true` if
     * the viewport changed; `false` if the id was evicted, already
     * visible, or already at the top. Requires `pane.read`.
     */
    scrollToRow(rowId: number): boolean;
    /**
     * Set (or clear with `null`) the OSC 133 command selection highlight.
     * `id` must refer to a live command in `pane.commands`; unknown ids are
     * silently treated as `null`. Requires `shell.commands`.
     */
    selectCommand(id: number | null): void;

    // `inject` is inherited from MbTerminal.

    /**
     * Write raw bytes to the PTY master (shell stdin). No bracketed-paste
     * wrapping. Use for synthetic keystrokes, OSC responses, or any
     * payload that isn't semantically a user paste. Requires `shell.write`.
     * Throws if no PTY.
     */
    write(data: string): void;
    /**
     * Paste text to the PTY master. When DECSET 2004 (bracketed paste) is
     * active, `data` is wrapped in `\x1b[200~`/`\x1b[201~`. Otherwise
     * behaves like `write()`. Requires `shell.write`. Throws if no PTY.
     */
    paste(data: string): void;
    /** Create a popup on this pane. Requires `ui.popup.create`. Returns null on failure. */
    createPopup(opts: { id: string; x: number; y: number; w: number; h: number }): MbPopup | null;

    /**
     * Create an inline embedded terminal anchored at the current cursor
     * row. `cols` matches this pane's cols; `rows` is caller-specified.
     * Returns `null` while the pane is on alt-screen, when `rows <= 0`,
     * or when an embedded already exists on the current cursor row.
     * Destroyed by explicit `close()` or eviction past the archive cap.
     * Requires `ui.popup.create`.
     */
    createEmbeddedTerminal(opts: { rows: number }): MbEmbeddedTerminal | null;

    /** Active embedded terminals on this pane. */
    readonly embeddeds: MbEmbeddedTerminal[];

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
    /**
     * Fired when the pane's effective title changes — either because the
     * shell wrote a new OSC 0/2 title (or popped the stack), or because
     * a script assigned to `pane.title`. Payload is the new effective
     * title (override if set, else OSC, else `""`).
     */
    addEventListener(event: "titleChanged", fn: (title: string) => void): void;
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
    /**
     * Fires when the pane enters or leaves the alternate screen (DECSET
     * 1049 toggle, or RIS reset while alt was active). Payload is the new
     * `usingAltScreen` boolean.
     */
    addEventListener(event: "altScreenChanged", fn: (usingAltScreen: boolean) => void): void;
    /**
     * Fires when a logical-line id is evicted past the archive cap.
     * After this, the id is invalid for any text/link/selection query
     * (calls return empty). Use to invalidate row-anchored state (prompt
     * markers, bookmarks, embedded anchors). Fires once per id, on the
     * main thread post-parse — handlers may call any synchronous pane
     * API. Requires `pane.read`.
     */
    addEventListener(event: "rowEvicted", fn: (ev: { rowId: number }) => void): void;

    removeEventListener(event: "input",  fn: (data: string) => string | void): void;
    removeEventListener(event: "output", fn: (data: string) => string | void): void;
    removeEventListener(event: "mouse",  fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
    removeEventListener(event: "foregroundProcessChanged", fn: (processName: string) => void): void;
    removeEventListener(event: "titleChanged", fn: (title: string) => void): void;
    removeEventListener(event: "mousemove", fn: (ev: { cellX: number; cellY: number; pixelX: number; pixelY: number }) => void): void;
    removeEventListener(event: "focusChanged", fn: (focused: boolean) => void): void;
    removeEventListener(event: "focusedPopupChanged", fn: (popupId: string | null) => void): void;
    removeEventListener(event: `osc:${number}`, fn: (payload: string) => void): void;
    removeEventListener(event: "commandComplete", fn: (cmd: MbCommand) => void): void;
    removeEventListener(event: "commandSelectionChanged", fn: (commandId: number | null) => void): void;
    removeEventListener(event: "rowEvicted", fn: (ev: { rowId: number }) => void): void;
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

interface MbPopup extends MbTerminal {
    /** Parent pane's tree-node UUID (stringified). */
    readonly paneId: string;
    /** Popup's caller-supplied string id (unique within its pane). */
    readonly id: string;
    /** True when this popup is its pane's focused popup. */
    readonly focused: boolean;
    readonly x: number;
    readonly y: number;

    /** Resize/move the popup. Requires `ui.popup.create`. */
    resize(opts: { x: number; y: number; w: number; h: number }): void;
    /** Focus this popup (clears the pane's focused embedded). Requires `ui.focus`. */
    focus(): boolean;
    /** Close and destroy the popup. Requires `ui.popup.destroy`. */
    close(): void;

    /** Keyboard events when the popup has focus. Requires `io.filter.input`. */
    addEventListener(event: "input", fn: (data: string) => void): void;
    /** Mouse press/release on the popup. Requires `ui`. */
    addEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    /**
     * Hover events — fires as the mouse cursor moves inside the popup's
     * rect. Coordinates are popup-local. Requires `ui`.
     */
    addEventListener(event: "mousemove", fn: (ev: {
        cellX: number; cellY: number; pixelX: number; pixelY: number;
    }) => void): void;
    /** Fires after a successful `resize({x,y,w,h})`. Payload: (cols, rows). */
    addEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    /** Fired once when the popup is closed. */
    addEventListener(event: "destroyed", fn: () => void): void;

    removeEventListener(event: "input", fn: (data: string) => void): void;
    removeEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "mousemove", fn: (ev: { cellX: number; cellY: number; pixelX: number; pixelY: number }) => void): void;
    removeEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
}

// ============================================================================
// Embedded terminal
// ============================================================================

/**
 * Headless child terminal anchored to a Document line id in its parent
 * pane, rendered inline at that row. Hidden while the parent is on
 * alt-screen; auto-destroyed (fires `"destroyed"`) when the anchor line
 * evicts past the archive cap.
 */
interface MbEmbeddedTerminal extends MbTerminal {
    /** Parent pane's tree-node UUID (stringified). */
    readonly paneId: string;
    /** Anchor line id — the stable Document line id where this embedded was created. */
    readonly id: number;
    /** True when this embedded is its pane's focused embedded (activeTerm points here). */
    readonly focused: boolean;

    /** Change the embedded's row count. Returns true on success. Requires `ui.popup.create`. */
    resize(rows: number): boolean;
    /** Focus this embedded (clears the pane's focused popup). Requires `ui.focus`. */
    focus(): boolean;
    /**
     * Destroy this embedded. Fires the `"destroyed"` event. Requires
     * `ui.popup.destroy`.
     */
    close(): void;

    /** Keystrokes delivered to the embedded while it has focus. Requires `io.filter.input`. */
    addEventListener(event: "input", fn: (data: string) => void): void;
    /**
     * Mouse press/release within the embedded's displaced band. Coordinates
     * are embedded-local (col 0 / row 0 = the embedded's top-left). Requires `ui`.
     */
    addEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    /** Hover events while the mouse is over this embedded. Requires `ui`. */
    addEventListener(event: "mousemove", fn: (ev: {
        cellX: number; cellY: number; pixelX: number; pixelY: number;
    }) => void): void;
    /**
     * Fires after a successful `resize(rows)`. Payload: (cols, rows).
     * Cols is the parent pane's width at the time. Does not fire on
     * parent-pane cols changes.
     */
    addEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    /** Fired once when the embedded is destroyed (either `close()` or anchor eviction). */
    addEventListener(event: "destroyed", fn: () => void): void;

    removeEventListener(event: "input", fn: (data: string) => void): void;
    removeEventListener(event: "mouse", fn: (ev: MbMouseEvent) => void): void;
    removeEventListener(event: "mousemove", fn: (ev: { cellX: number; cellY: number; pixelX: number; pixelY: number }) => void): void;
    removeEventListener(event: "resized", fn: (cols: number, rows: number) => void): void;
    removeEventListener(event: "destroyed", fn: () => void): void;
}

// ============================================================================
// Tab identity
// ============================================================================
// Tabs have no JS class — identity is the subtreeRoot UUID string. To inspect
// or operate on a tab:
//   - `mb.layout.queryNodes("Stack", root)` enumerates tab subtree roots
//   - `mb.layout.node(tabUuid)` returns layout-tree info for the tab
//   - `mb.layout.queryNodes("Terminal", tabUuid)` lists the tab's terminals
//   - `mb.pane(termUuid)` constructs Pane objects from those terminal UUIDs
//   - `mb.layout.activateTab(tabUuid)` / `mb.layout.closeTab(tabUuid)` mutate

// ============================================================================
// Script loading result
// ============================================================================

/**
 * Outcome of `mb.loadScript` / `mb.approveScript`. Narrow via `status`:
 * `id` is present only on `"loaded"`; `error` only on `"error"`.
 */
type MbLoadResult =
    | { status: "loaded"; id: number }
    | { status: "pending" }
    | { status: "denied" }
    | { status: "error"; error?: string };

// ============================================================================
// Config snapshot
// ============================================================================

interface MbConfigPadding { left: number; top: number; right: number; bottom: number; }

interface MbConfigCursor {
    shape: "block" | "underline" | "bar";
    blink: boolean;
    blink_rate: number;
    blink_fps: number;
}

interface MbConfigColors {
    foreground: string;
    background: string;
    cursor: string;
    color0:  string; color1:  string; color2:  string; color3:  string;
    color4:  string; color5:  string; color6:  string; color7:  string;
    color8:  string; color9:  string; color10: string; color11: string;
    color12: string; color13: string; color14: string; color15: string;
}

interface MbConfigTabBarColors {
    background:  string;
    active_bg:   string;
    active_fg:   string;
    inactive_bg: string;
    inactive_fg: string;
    /** Bg of a tab being dragged. */
    drag_bg:     string;
    /** Fg of a tab being dragged. */
    drag_fg:     string;
}

interface MbConfigTabBar {
    style: "auto" | "visible" | "hidden";
    /** Where the chrome tab bar sits relative to the document area. */
    position: "top" | "bottom";
    font: string;
    font_size: number;
    max_title_length: number;
    progress_icon: boolean;
    progress_bar: boolean;
    progress_color: string;
    progress_height: number;
    /** Px before tab click → drag. */
    drag_threshold_px: number;
    colors: MbConfigTabBarColors;
}

interface MbConfigNotifications { show_when_foreground: boolean; }

/** One entry in `config.keybinding[]`. */
interface MbKeybinding {
    /**
     * Single key (`"ctrl+shift+t"`) or sequence (`["ctrl+x", "ctrl+c"]`)
     * for multi-stroke bindings.
     */
    keys: string[];
    /** Snake-case action name, optionally namespaced (`"namespace.action"`). */
    action: string;
    /** Action-specific positional arguments (e.g. `["right"]`). */
    args?: string[];
}

/** One entry in `config.mousebinding[]`. */
interface MbMousebinding {
    button: "left" | "middle" | "right";
    event: "press" | "release" | "click" | "doublepress" | "triplepress" | "drag";
    /** Defaults to `"ungrabbed"`. */
    mode?: "ungrabbed" | "grabbed" | "any";
    /** Defaults to `"any"`. */
    region?: "any" | "tab_bar" | "pane" | "divider";
    action: string;
    args?: string[];
}

/**
 * Live snapshot of the parsed TOML config. Read via `mb.config`. Includes
 * mutation methods (see `MbConfigMutations`) gated on `config.modify`.
 * Capturing into a local works: `const cfg = mb.config; cfg.patch({...})`.
 */
interface MbConfig extends MbConfigMutations {
    font: string;
    font_size: number;
    bold_strength: number;
    /**
     * Startup-only: hot-reload / `patch` updates the snapshot but does
     * NOT resize existing panes' scrollback rings, and new panes spawned
     * post-reload still use the startup value.
     */
    scrollback_lines: number;
    padding: MbConfigPadding;
    cursor: MbConfigCursor;
    colors: MbConfigColors;
    tab_bar: MbConfigTabBar;
    keybinding: MbKeybinding[];
    mousebinding: MbMousebinding[];
    divider_color: string;
    divider_width: number;
    /** Hit-test pad on each side of a split divider, in px. */
    divider_hit_pad: number;
    inactive_pane_tint: string;
    inactive_pane_tint_alpha: number;
    active_pane_tint: string;
    active_pane_tint_alpha: number;
    replacement_char: string;
    command_outline_color: string;
    command_dim_factor: number;
    alt_sends_esc: boolean;
    command_navigation_wrap: boolean;
    key_sequence_timeout_ms: number;
    /**
     * Color preference reported for mode 2031 / DSR-997. `"auto"` defers
     * to the system; `"light"`/`"dark"` overrides bypass system query.
     */
    color_scheme: "auto" | "light" | "dark";
    /**
     * Whether `closePane`/`closeTab`/OS window-close prompts before
     * killing. `"never"` closes immediately; `"if_busy"` (default) prompts
     * when any non-shell foreground process is running; `"always"` always
     * prompts. The "shell" list is JS-side state in default-ui.js; mutate
     * via `default-ui.add-shell` / `default-ui.remove-shell` actions.
     */
    confirm_close: "never" | "if_busy" | "always";
    notifications: MbConfigNotifications;
    /** Prefix prepended to the OS window title. */
    window_title_prefix: string;
}

/**
 * Runtime config mutators (gated on `config.modify`). All mutations are
 * ephemeral — last-write-wins against concurrent disk edits, nothing is
 * persisted to `config.toml`. Throws `Error` on validation failure.
 */
interface MbConfigMutations {
    /**
     * Deep-merge a partial config. Plain-object fields recurse; arrays
     * (`keybinding`, `mousebinding`) and primitives are replaced wholesale
     * — use `addKeybinding`/`addMousebinding` to extend without replacing.
     *
     * @example mb.config.patch({ font_size: 14, tab_bar: { position: "top" } });
     */
    patch(partial: Partial<MbConfig>): void;

    /**
     * Append a single keybinding. Does not deduplicate; later entries win.
     * Cannot remove built-in default bindings (those aren't in
     * `config.keybinding[]`).
     */
    addKeybinding(b: MbKeybinding): void;

    /**
     * Remove every entry whose `keys` array exactly matches (element-wise,
     * order-sensitive). Returns count removed.
     */
    removeKeybinding(match: { keys: string[] }): number;

    /** Append a single mousebinding. */
    addMousebinding(b: MbMousebinding): void;

    /**
     * Remove mousebindings matching the given fields. Omitted fields are
     * wildcards. Returns count removed.
     */
    removeMousebinding(match: {
        button?: string;
        event?: string;
        mode?: string;
        region?: string;
    }): number;
}

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

/**
 * Array returned by `mb.actions`, augmented with handler-registry methods.
 * Capturing into a local works; the array regenerates on every access.
 */
interface MbActions extends ReadonlyArray<MbActionInfo> {
    /** Register a handler for `name`. Requires `layout.modify`. */
    register(name: string, fn: (...args: string[]) => void): void;
    /** Drop a previously-registered handler. Requires `layout.modify`. */
    unregister(name: string): void;
}

/**
 * Payload for `terminalExited`. Emitted after a Terminal node's PTY child
 * exits. The Terminal node remains in the tree so the controller can
 * decide whether to remove it, transform the tab, or respawn.
 */
interface MbTerminalExitedEvent {
    /** Stringified pane handle (UUID). */
    readonly paneId: string;
    /** Stable layout-tree node id of the now-empty Terminal node, or `null`. */
    readonly paneNodeId: string | null;
}

// ============================================================================
// mb global
// ============================================================================

interface MbGlobal {
    /** The currently focused pane, or undefined if none. */
    readonly activePane: MbPane | undefined;
    /**
     * Construct a Pane object wrapping the live Terminal at `nodeId`. Returns
     * `null` when the UUID is malformed or doesn't refer to a live Terminal.
     */
    pane(nodeId: string): MbPane | null;
    /**
     * All available actions (built-in + script-registered), with `register` /
     * `unregister` methods attached. Regenerated on every read.
     */
    readonly actions: MbActions;
    /**
     * Layout-tree primitives: containers, stacks, tab bars, terminal spawn,
     * slot constraints, and queries. Mutating methods require `layout.modify`.
     */
    readonly layout: MbLayout;
    /**
     * Live snapshot of the loaded TOML config. Re-read after the
     * `configChanged` event to pick up hot-reload updates.
     *
     * The returned object is freshly built on every getter access —
     * mutating individual fields has no effect. To change config from JS
     * use the mutation methods on the returned object (`patch`,
     * `addKeybinding`, etc. — see `MbConfigMutations`).
     */
    readonly config: MbConfig;

    // --- Actions ---
    invokeAction(name: string, ...args: string[]): boolean;
    /** Set this script's action namespace. Can only be called once per instance. */
    setNamespace(namespace: string): boolean;
    /** Register `<namespace>.<name>` as a script action. */
    registerAction(name: string): boolean;

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
    /** Quit the application. */
    quit(): void;
    /** Non-destructive permission query (unknown name → false). Group names require all bits. */
    hasPermission(name: MbPermission): boolean;

    // --- Tokens / crypto ---
    /**
     * Generate `length` cryptographically-secure random bytes as a
     * `2 * length`-character hex string. Defaults to 32 bytes (64 chars).
     * Ungated.
     */
    createSecureToken(length?: number): string;
    /** Generate a random UUID v4 (36-char canonical string). Ungated. */
    createUuid(): string;

    // --- Custom terminal capabilities ---
    /** Register a custom XTGETTCAP capability. */
    registerTcap(name: string, value: string): void;
    /** Remove a custom XTGETTCAP capability. */
    unregisterTcap(name: string): void;

    // --- Clipboard ---
    /**
     * Read from the system clipboard. Async to avoid blocking the main
     * thread on X11's SELECTION_NOTIFY round-trip. Requires `clipboard.read`.
     * @param source `"clipboard"` (default) or `"primary"` (X11 primary selection).
     */
    getClipboard(source?: "clipboard" | "primary"): Promise<string>;
    /**
     * Write to the system clipboard. Requires `clipboard.write`.
     * @param source `"clipboard"` (default) or `"primary"` (X11 primary selection).
     */
    setClipboard(text: string, source?: "clipboard" | "primary"): void;

    /**
     * Begin an animation on a property of `target`. v1 only accepts
     * `MbDecorationHandle`. Equivalent to `target.animate(prop, params)`.
     */
    startAnimation(target: MbDecorationHandle, prop: MbDecorationProp,
                   params: MbEaseAnimParams): MbAnimationHandle;

    // --- External process management ---
    /**
     * External (non-PTY) process APIs. Use for editors, GUI tools, OS
     * integrations — anything to run alongside mb rather than inside a
     * pane. For pane shell interaction, use `pane.write`/`pane.paste`
     * instead.
     */
    readonly process: MbProcess;

    /**
     * Compile-time host identity, Node-style. Vocabulary matches Node's
     * `process.platform` / `os.arch()`.
     */
    readonly os: {
        /** `"darwin" | "linux" | "win32" | "freebsd" | "openbsd" | "unknown"` */
        readonly platform: string;
        /** `"arm64" | "x64" | "ia32" | "arm" | "unknown"` */
        readonly arch: string;
        /**
         * Captured at startup; later hostname changes are not reflected.
         * Empty string on `gethostname` failure.
         */
        readonly hostname: string;
    };

    // --- Lifecycle events ---
    /** Fires once per new pane. Fires on every loaded instance. */
    addEventListener(event: "paneCreated", fn: (pane: MbPane) => void): void;
    /**
     * Fires after a pane has been destroyed. Payload is `(paneId,
     * paneNodeId)` — the pane handle is no longer usable. `paneNodeId` is
     * `null` if the pane had already detached from the tree.
     */
    addEventListener(
        event: "paneDestroyed",
        fn: (paneId: string, paneNodeId: string | null) => void
    ): void;
    /**
     * Fires when a Stack-direct-child becomes a tab. Top-level tab creation
     * fires once per `createTab()`; sub-tab creation fires from
     * `wrapInStack` (first sub-tab content) and `createSubTab`.
     *
     * Payload fields:
     *   - `id`: the new tab/sub-tab's node UUID
     *   - `parentStackId`: UUID of the enclosing Stack
     *   - `level`: `"top"` if `parentStackId` is the root tabs Stack,
     *     `"sub"` otherwise. Filter on this if your listener only cares
     *     about top-level tabs.
     */
    addEventListener(
        event: "tabCreated",
        fn: (info: { id: string; parentStackId: string; level: "top" | "sub" }) => void
    ): void;
    /**
     * Fires after a Stack-direct-child has been closed via `closeTab`
     * (top-level or sub-tab). Payload shape matches `tabCreated`.
     */
    addEventListener(
        event: "tabDestroyed",
        fn: (info: { id: string; parentStackId: string; level: "top" | "sub" }) => void
    ): void;
    /**
     * Fires when a Terminal node's child process exits. The Terminal node
     * is left in place — the controller decides what to do (remove node,
     * transform the tab, respawn).
     */
    addEventListener(event: "terminalExited", fn: (ev: MbTerminalExitedEvent) => void): void;
    /** Fires when the persisted config has been reloaded from disk. */
    addEventListener(event: "configChanged", fn: () => void): void;
    /**
     * OS-level window close (X button / NSApp termination / Cmd+Q). When
     * any listener is registered, C++ defers the quit and fires the
     * event; the listener owns the decision and must call `mb.quit()` to
     * commit. With no listener, mb quits immediately. default-ui.js
     * listens by default to drive close-confirm.
     */
    addEventListener(event: "quit-requested", fn: () => void): void;
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
    removeEventListener(
        event: "paneDestroyed",
        fn: (paneId: string, paneNodeId: string | null) => void
    ): void;
    removeEventListener(
        event: "tabCreated",
        fn: (info: { id: string; parentStackId: string; level: "top" | "sub" }) => void
    ): void;
    removeEventListener(
        event: "tabDestroyed",
        fn: (info: { id: string; parentStackId: string; level: "top" | "sub" }) => void
    ): void;
    removeEventListener(event: "terminalExited", fn: (ev: MbTerminalExitedEvent) => void): void;
    removeEventListener(event: "configChanged", fn: () => void): void;
    removeEventListener(event: "action", fn: (actionName: string) => void): void;
    removeEventListener(event: "action", actionName: string, fn: (...args: string[]) => void): void;
    removeEventListener(
        event: "scriptPermissionRequired",
        fn: (path: string, permissions: string, hash: string) => void
    ): void;

    /**
     * Bulk-remove listeners installed on `mb` via `addEventListener`.
     *
     *  - `removeAllListeners()` — clear every listener across every event
     *    family on `mb`, including all per-action handlers.
     *  - `removeAllListeners(event)` — clear listeners for a single
     *    event. `event === "action"` is a wildcard: clears every
     *    per-action listener at once.
     *  - `removeAllListeners("action", actionName)` — clear only the
     *    listeners registered for that specific action.
     *
     * Listeners installed on sub-objects (`pane.addEventListener`,
     * `popup.addEventListener`, …) live on their own registries and are
     * not affected.
     */
    removeAllListeners(): void;
    removeAllListeners(
        event:
            | "paneCreated"
            | "paneDestroyed"
            | "tabCreated"
            | "tabDestroyed"
            | "terminalExited"
            | "configChanged"
            | "quit-requested"
            | "action"
            | "scriptPermissionRequired"
    ): void;
    removeAllListeners(event: "action", actionName: string): void;
}

// ============================================================================
// mb.layout — LayoutTree primitives
// ============================================================================

/** Per-child slot constraints inside a Container. */
interface MbChildSlotOptions {
    /** Relative growth weight when distributing leftover space. Default 1. */
    stretch?: number;
    /** Lower bound on the child's allocation, in cells. Default 0 = unset. */
    minCells?: number;
    /** Upper bound on the child's allocation, in cells. Default 0 = unset. */
    maxCells?: number;
    /** Pin the child to an exact cell count. Default 0 = unset. */
    fixedCells?: number;
}

interface MbChildSlot {
    /** UUID of the child node. */
    readonly id: string;
    readonly stretch: number;
    readonly minCells: number;
    readonly maxCells: number;
    readonly fixedCells: number;
}

interface MbRect {
    readonly x: number;
    readonly y: number;
    readonly w: number;
    readonly h: number;
}

interface MbContainerNode {
    readonly id: string;
    readonly kind: "container";
    readonly label: string;
    readonly parent: string | null;
    readonly direction: "horizontal" | "vertical";
    readonly children: MbChildSlot[];
}

interface MbStackNode {
    readonly id: string;
    readonly kind: "stack";
    readonly label: string;
    readonly parent: string | null;
    readonly children: MbChildSlot[];
    readonly activeChild: string | null;
    readonly opaque: boolean;
    readonly zoomTarget: string | null;
}

interface MbTabBarNode {
    readonly id: string;
    readonly kind: "tabbar";
    readonly label: string;
    readonly parent: string | null;
    readonly boundStack: string | null;
}

interface MbTerminalNode {
    readonly id: string;
    readonly kind: "terminal";
    readonly label: string;
    readonly parent: string | null;
}

type MbLayoutNode = MbContainerNode | MbStackNode | MbTabBarNode | MbTerminalNode;

/** Focused-pane snapshot returned by `focusedPane()`. */
interface MbFocusedPaneInfo {
    /** The focused pane's Terminal-node UUID. */
    readonly nodeId: string;
    /**
     * The enclosing TOP-LEVEL tab's subtreeRoot UUID, or `null` if not in
     * a tab. For panes inside a sub-bar, this is still the top-level tab
     * (not the sub-tab Container) — walk up via `nearestAncestorOfKind`
     * for the sub-stack level.
     */
    readonly tabNodeId: string | null;
}

/**
 * `splitPane` direction. Cardinal forms (`"left"`/`"right"`/`"up"`/`"down"`)
 * imply orientation AND placement of the new pane relative to the existing
 * one; the orientation-only forms place the new pane second by default.
 */
type MbSplitDir = "horizontal" | "vertical" | "h" | "v" | "left" | "right" | "up" | "down";

interface MbLayout {
    // --- Node creation (UUID returned). All require `layout.modify`. ---

    /**
     * Spawn a PTY child terminal and attach the resulting Terminal node under
     * `parentNodeId` (a Container or Stack). Returns the new pane's UUID
     * string, or `null` if the spawn failed.
     */
    createTerminal(parentNodeId: string, opts?: { cwd?: string }): string | null;
    /** Create a free-floating Container node. Returns its UUID. */
    createContainer(direction?: "horizontal" | "vertical" | "h" | "v"): string;
    /** Create a free-floating Stack node. Returns its UUID. */
    createStack(): string;
    /** Create a free-floating TabBar node. Returns its UUID. */
    createTabBar(): string;

    // --- Tab lifecycle (route through Platform so PTY/graveyard stay in sync) ---

    /**
     * Create an empty top-level tab (a Stack added as a direct child of
     * the root tabs Stack). Returns the new tab's subtreeRoot UUID, or
     * `null` on failure. Follow with `createTerminal(returnedId, ...)` to
     * spawn an initial pane.
     */
    createTab(): string | null;
    /**
     * Create a sub-tab inside `parentStackUuid` (must be a Stack —
     * typically `wrapInStack(...).stack`). Appends a Container as the
     * Stack's active child and fires `tabCreated` with `level: "sub"`.
     * Returns the new Container's UUID. Follow with
     * `createTerminal(returnedId, ...)` to spawn a pane.
     */
    createSubTab(parentStackUuid: string, opts?: { dir?: MbSplitDir }): string | null;
    /**
     * Close a Stack-direct-child (top-level tab or sub-bar tab). Walks up
     * to the enclosing Stack, tears down panes, activates a surviving
     * sibling, restores its remembered focus. Returns `false` if the UUID
     * isn't a direct child of any Stack or the Stack would be left empty
     * (refuses last tab).
     */
    closeTab(nodeId: string): boolean;
    /**
     * Make `nodeId` the active child of its enclosing Stack. Works for
     * top-level tabs and sub-bar tabs. Top-level activation also updates
     * window title and focus tracking.
     */
    activateTab(nodeId: string): void;

    // --- Pane lifecycle ---

    /** Move keyboard focus to the pane identified by its tree node id. */
    focusPane(nodeId: string): boolean;
    /**
     * Remove a non-Terminal subtree (Container, Stack, or already-empty Terminal
     * leaf). Refuses if any descendant Terminal is still live — call
     * `killTerminal` first for those.
     */
    removeNode(nodeId: string): boolean;
    /**
     * Synchronously kill a Terminal's PTY child. The Terminal is graveyarded;
     * the tree node is left in place so the controller can decide whether to
     * remove it (or transform the tab) from the `terminalExited` event.
     */
    killTerminal(nodeId: string): boolean;
    /**
     * Split an existing pane. The direction string accepts orientation aliases
     * (`"horizontal"`/`"h"` and `"vertical"`/`"v"`) plus cardinal placements
     * (`"left"`/`"right"` for horizontal, `"up"`/`"down"` for vertical).
     * `newIsFirst` defaults to `false` for orientation-only and right/down forms,
     * `true` for left/up. The boolean argument, when provided, OR's into that
     * default. Returns the new pane's UUID string, or `null` on failure.
     */
    splitPane(existingNodeId: string, dir: MbSplitDir, newIsFirst?: boolean): string | null;

    /**
     * Wrap an existing node in
     *   `Container[dir] = [TabBar | Stack[Container[horizontal, existing]]]`
     * (TabBar leading iff `tabBarFirst`). The inner Container is the
     * "tab content" slot — `createTerminal(stack)` descends into the
     * active tab content. Slot weights of `existingNodeId` transfer to
     * the new wrapper so the parent layout doesn't lurch. Returns
     * `{wrapper, stack, tabBar, content}`; throws on validation failure.
     *
     * Defaults: `dir = "vertical"`, `tabBarFirst = true` (horizontal tab
     * strip above content).
     */
    wrapInStack(
        existingNodeId: string,
        opts?: { dir?: "horizontal" | "vertical" | "h" | "v"; tabBarFirst?: boolean }
    ): { wrapper: string; stack: string; tabBar: string; content: string };

    // --- Slot constraints ---

    setSlotStretch(parent: string, child: string, stretch: number): boolean;
    setSlotMinCells(parent: string, child: string, minCells: number): boolean;
    setSlotMaxCells(parent: string, child: string, maxCells: number): boolean;
    setSlotFixedCells(parent: string, child: string, fixedCells: number): boolean;

    /**
     * Adjust a pane's allocation along `dir`. `dir` is one of `"left"`,
     * `"right"`, `"up"`, `"down"`; `amount` is in cells (negative shrinks).
     */
    adjustPaneSize(paneNodeId: string, dir: "left" | "right" | "up" | "down", amount: number): boolean;

    // --- Stack zoom ---

    /**
     * Pin a Stack to render only `targetNodeId`. Pass `null`/omit to clear
     * the zoom and restore normal rendering.
     */
    setStackZoom(stackNodeId: string, targetNodeId?: string | null): boolean;
    /**
     * Mark a Stack as opaque to focus traversal — focus-arrow keybindings
     * treat it as a single unit instead of recursing into children. Pure
     * navigation hint; layout unchanged. Returns `false` if `stackNodeId`
     * isn't a Stack.
     */
    setStackOpaque(stackNodeId: string, opaque: boolean): boolean;

    // --- Tree mutation primitives ---

    /** Promote a free-floating subtree to root. */
    setRoot(nodeId: string): void;
    /** Returns the root node id, or `null` if there is none. */
    getRoot(): string | null;
    /** Append `child` under `parent` with optional slot constraints. */
    appendChild(parent: string, child: string, opts?: MbChildSlotOptions): void;
    removeChild(parent: string, child: string): void;
    /**
     * Replace `oldChild` in `parent`'s child list with `newChild` (same
     * position). `newChild` must be detached. Throws on parent missing,
     * `oldChild` not in parent, or `newChild` already having a parent.
     */
    replaceChild(parent: string, oldChild: string, newChild: string, opts?: MbChildSlotOptions): void;
    /**
     * Move `child` within `parent` by `delta` positions (no wraparound —
     * use `rotateChildren` for that). Returns `false` on out-of-bounds.
     * Stack `activeChild` follows the moved entry.
     */
    moveChild(parent: string, child: string, delta: number): boolean;
    /**
     * Circularly shift every child of `parent` by `delta` positions.
     * Returns `false` if `parent` isn't a Container/Stack or has fewer
     * than two children.
     */
    rotateChildren(parent: string, delta: number): boolean;
    /**
     * Swap two leaf nodes anywhere in the tree, including across parents.
     * Slot weights stay with the position so the visual layout doesn't
     * change — only the leaves move. Refuses nil/equal ids, missing or
     * detached nodes, or pairs where one is an ancestor of the other.
     */
    swapLeaves(a: string, b: string): boolean;
    /**
     * Walk up from `fromParent` and collapse each Container with exactly
     * one child (the lone child is promoted into the grandparent's slot,
     * the Container is destroyed). Stops at `stopAt` (exclusive) or when
     * reaching a node with 0 or ≥2 children.
     */
    collapseSingletonsAbove(fromParent: string, stopAt: string): void;
    /** Set the active child of a Stack. */
    setActiveChild(stack: string, child: string): void;
    /** Bind a TabBar to a Stack (or pass `null`/omit to clear the binding). */
    setTabBarStack(tabBar: string, stackOrNull: string | null | undefined): void;
    /** Set the human-readable label on a node. */
    setLabel(nodeId: string, label: string): void;
    /** Drop a node from the tree (no recursion guard — see `removeNode` for safer leaf removal). */
    destroyNode(nodeId: string): void;

    // --- Queries / introspection (ungated) ---

    /** Return the node record for `id`, or `null` if unknown. */
    node(id: string): MbLayoutNode | null;
    /**
     * Enumerate every node of `kind` reachable from `subtreeRoot` (defaults
     * to the tree root). Returned UUIDs are in tree-walk order
     * (implementation-defined within that).
     */
    queryNodes(
        kind: "Terminal" | "Container" | "Stack" | "TabBar"
            | "terminal" | "container" | "stack" | "tabbar",
        subtreeRoot?: string | null
    ): string[];
    /**
     * Walk `start`'s parent chain (inclusive) and return the first
     * ancestor of `kind`. `null` if none.
     */
    nearestAncestorOfKind(
        start: string,
        kind: "Terminal" | "Container" | "Stack" | "TabBar"
            | "terminal" | "container" | "stack" | "tabbar"
    ): string | null;
    /**
     * Walk `descendant`'s parent chain and return whether any ancestor
     * equals `ancestor`. Reflexive: `contains(x, x)` is `true`.
     */
    contains(ancestor: string, descendant: string): boolean;
    /**
     * Every Terminal-kind leaf UUID inside `start`'s subtree. With
     * `onlyActiveStack: true`, Stack recursion follows `activeChild` only
     * (visible-on-screen). Defaults to `false` (every terminal regardless
     * of visibility).
     */
    terminalLeavesIn(start: string, onlyActiveStack?: boolean): string[];
    /**
     * First node whose `label` exactly equals `label`, or `null`. Empty
     * strings never match.
     */
    findByLabel(label: string): string | null;
    /**
     * UUID of the pane last focused inside `subtreeRoot`, if it still
     * exists in the subtree. `null` otherwise.
     */
    rememberedFocusInSubtree(subtreeRoot: string): string | null;
    /**
     * Compute pixel rects for every node given the window rect and
     * per-cell pixel dimensions. Returns a UUID-keyed map.
     */
    computeRects(window: MbRect, cellW?: number, cellH?: number): { [nodeId: string]: MbRect };
    /** Snapshot of the focused pane and its enclosing tab, or `null`. */
    focusedPane(): MbFocusedPaneInfo | null;
}

// ============================================================================
// Globals
// ============================================================================

declare const mb: MbGlobal;
// Host-provided timer globals. These are NOT WHATWG-spec timers — the
// callback receives no arguments, ids are plain numbers (not Node's
// opaque `Timeout` object), and `setInterval`'s `ms` is required (the
// runtime returns nothing useful when it is missing rather than
// defaulting). Implementations live in `src/script/ScriptEngine.cpp`.

/** Schedule `fn` to run once after `ms` (default 0). Returns the timer id. */
declare function setTimeout(fn: () => void, ms?: number): number;
/** Cancel a pending `setTimeout`. Unknown ids are silently ignored. */
declare function clearTimeout(id: number): void;
/** Schedule `fn` to fire every `ms` (clamped to a 1 ms minimum). Returns the timer id. */
declare function setInterval(fn: () => void, ms: number): number;
/** Cancel a `setInterval`. Unknown ids are silently ignored. */
declare function clearInterval(id: number): void;

// Host console. Each method stringifies its arguments (joined by a single
// space) and writes them to spdlog under the `[js]` logger. Level map:
// trace -> trace, debug -> debug, info / log -> info, warn -> warn,
// error -> error. `group` / `groupCollapsed` indent subsequent output by
// two spaces per nesting level; `groupEnd` unindents. Implementations
// live in `src/script/ScriptEngine.cpp`.
interface MbConsole {
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
    debug(...args: unknown[]): void;
    trace(...args: unknown[]): void;
    group(...args: unknown[]): void;
    groupCollapsed(...args: unknown[]): void;
    groupEnd(): void;
}
declare const console: MbConsole;

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
