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
    /**
     * Launch external (non-PTY) processes via `mb.process.spawn`.
     * Materially different threat model from `shell.write` (bounded
     * to the current pane's PTY) and `io.inject` (synthetic terminal
     * input only): this bit grants execution of arbitrary binaries
     * with the user's environment. Held separate so popup-only or
     * shell-only scripts don't pick it up implicitly.
     */
    | "process.spawn"
    /**
     * Shorthand for every regular permission bit. Equivalent to
     * listing all the groups + the standalone bits (currently
     * `ui.focus`, `pane.read`). Symmetric with how mb's allowlist
     * file represents a fully-permissive grant.
     *
     * Does NOT include the `"builtin"` elevation marker — `"all"`
     * grants every privilege a *user script* can hold, but the
     * loaded instance still runs sandboxed (file paths confined,
     * allowlist prompt fires, etc.). For the trust elevation, use
     * `"builtin"` instead (and see its documentation for the
     * caller-restriction).
     */
    | "all"
    /**
     * Privilege-elevation request: load the target script as a
     * built-in. A built-in instance has every permission bit AND
     * runs without sandbox restrictions — unrestricted file paths,
     * no allowlist prompt, full trust. Same level as the shipped
     * controller scripts and `~/.config/MasterBandit/config.js`.
     *
     * **Caller restriction**: only built-in scripts may name this
     * permission in their `mb.loadScript` call. A user script that
     * tries it triggers a permission violation and the calling
     * instance is terminated — same kill-on-attempt semantics as
     * accessing `shell.commands` without the grant. The rationale:
     * a built-in can already get the same effect by `import`-ing
     * arbitrary files from its directory (which run with the
     * importer's permissions in the same JS context), so explicitly
     * letting them spawn separate built-in instances doesn't grant
     * new authority — just lifecycle control. User scripts must
     * never escape their sandbox, so the attempt is a hard error.
     *
     * Implies `"all"`; listing them together is redundant but
     * harmless.
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
     * Working directory for the spawned process. Empty string or
     * omitted → inherit mb's cwd. The chdir is performed in the
     * grandchild *after* fork but *before* exec; failure aborts the
     * spawn (the child `_exit(127)`s and the script-side return
     * stays the intermediate-child pid — there's no synchronous error
     * channel for post-fork failures in v1).
     */
    cwd?: string;
    /**
     * Environment variable overrides. Each key in this object replaces
     * (or appends, if absent) the corresponding entry in the inherited
     * environment. Keys absent here keep their inherited value. There
     * is no syntax for *removing* an inherited key in v1 — workaround
     * is to set it to the empty string.
     *
     * Values must be strings; non-string entries are silently dropped.
     */
    env?: { [key: string]: string };
}

interface MbProcess {
    /**
     * Launch an external process detached from mb. Fire-and-forget:
     * the spawned process runs as its own session leader (`setsid(2)`),
     * does not inherit mb's tty or stdio (stdin/stdout/stderr are
     * reopened to `/dev/null`), and is reaped by init/launchd rather
     * than mb. There is no exit-code or output-capture surface in v1
     * — if you need to wait on the process or read its output, use a
     * terminal pane via `mb.layout.createTerminal` instead.
     *
     * Argv semantics: argv[0] is set to `path` automatically. The
     * `args` array becomes argv[1..N] as the child sees it. No shell
     * is involved — `path` is resolved via `execvp(3)` PATH lookup,
     * and arguments are passed verbatim with no metacharacter
     * expansion or quoting. This is intentional: shell-style commands
     * are an injection vector when scripts splice user input.
     *
     * @param path  Program to execute (basename or absolute path).
     * @param args  argv[1..N]. Defaults to `[]` (program runs with no
     *              arguments, which is what most launchers want).
     * @param opts  Optional cwd / env overrides. See
     *              `MbProcessSpawnOptions`.
     * @returns     The intermediate-child pid on successful fork,
     *              or `0` on pre-fork failure (e.g. resource limits).
     *              Note this is *not* the grandchild pid that actually
     *              runs the binary — there's no portable way to surface
     *              that without breaking the detached double-fork
     *              pattern. v1 has no API for tracking, signalling, or
     *              waiting on the spawned process; if those are added,
     *              they'll come as a separate `spawnTracked` returning
     *              a handle.
     *
     * @throws TypeError on missing args, invalid arg types, or if the
     *         caller lacks `process.spawn` permission.
     *
     * Does NOT throw on grandchild exec failure (e.g. binary not found
     * in PATH). The failure is logged from the C++ side via spdlog,
     * but the script just sees the spawn return a pid and proceed —
     * this is a fundamental tradeoff of the detached model. If you
     * need exec-success confirmation, validate the binary exists with
     * a synchronous `fs.statSync` first, or use a tracked spawn (when
     * available).
     *
     * @example
     *   // Open the user's $EDITOR on a file
     *   mb.process.spawn(process.env.EDITOR ?? "vi", ["/etc/hosts"]);
     *
     *   // Run a linter in a specific directory with extra env
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
 * Format used to encode captured PTY output.
 *
 * - `"raw"` writes every byte the emulator received, verbatim. The
 *   resulting file contains all ANSI/CSI/OSC escapes and binary
 *   bytes; viewers like `less -R` or `cat` (with reset on exit)
 *   replay it close to what the user saw. Smallest format, no
 *   per-record overhead.
 * - `"text"` writes escape-stripped plain text suitable for direct
 *   consumption by an LLM, log scraper, or human reading the file
 *   without an interpreter. ANSI/CSI/OSC/DCS/SS3 sequences are
 *   dropped along with C0 control characters other than `\n` and
 *   `\t`. CR is normalised: `\r\n` becomes `\n` and bare `\r` is
 *   dropped (in-place updates like progress bars produce repeated
 *   content but no cursor-driven overwrites — modelling overwrites
 *   would require a full emulator). UTF-8 high bytes pass through
 *   verbatim. Stripper state persists across PTY chunks so a CSI
 *   split mid-sequence still gets dropped correctly.
 * - `"asciicast"` writes asciicast v2 (https://docs.asciinema.org).
 *   A header line at open carries terminal dimensions and the
 *   wall-clock timestamp; each subsequent PTY chunk becomes one
 *   `[<elapsed_seconds>, "o", <chunk>]` JSON-lines record.
 *   Replayable with the asciinema toolchain. Larger than raw because
 *   of JSON framing, but seekable + tooling-rich.
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
 * - `"explicit"`: the script called `.stop()` on the handle, OR the
 *   owning script instance was unloaded (cleanup sweep). `error`
 *   is the empty string in this case.
 * - `"io-error"`: a write to the destination file failed (disk full,
 *   EIO, fd revoked, etc.). The capture has been auto-stopped and
 *   the FILE* closed; the `error` field carries the strerror text
 *   from the failing write.
 */
type MbOutputCaptureStopReason = "explicit" | "io-error";

/** Payload of the `stopped` event on `MbOutputCapture`. */
interface MbOutputCaptureStoppedEvent {
    reason: MbOutputCaptureStopReason;
    /** strerror text on `io-error`; empty string otherwise. */
    error: string;
}

/**
 * Handle returned by `pane.captureOutputToFile`. Carries enough
 * identifying state for the script to reason about the capture, but
 * the underlying file handle is owned by the C++ Terminal. The
 * handle stays alive after `.stop()` (or auto-stop) — methods just
 * become no-ops; `active` is the canonical "still capturing?" flag.
 *
 * Multiple captures per pane are supported, but each must point at
 * a distinct path. Calling `pane.captureOutputToFile` twice with
 * the same path on the same pane throws.
 */
interface MbOutputCapture {
    /** Sandbox-validated absolute path being written to. */
    readonly path: string;
    /** Format passed at registration time. */
    readonly format: MbOutputCaptureFormat;
    /**
     * `true` until the capture stops. Flips to `false` immediately
     * BEFORE the `stopped` event fires, so listeners that check
     * `handle.active` from inside the callback see the post-stop
     * state. Idempotent stops (calling `.stop()` on an already-
     * stopped handle) leave it `false`.
     */
    readonly active: boolean;
    /** UUID string of the pane this capture is attached to. */
    readonly paneId: string;

    /**
     * Stop this capture. Closes the file and fires the `stopped`
     * event with reason `"explicit"`. Idempotent: returns `true`
     * the first time it actually closed a live capture, `false`
     * if the handle was already stopped (either by a prior
     * `.stop()`, an auto-stop on I/O error, or the owning
     * instance's unload sweep).
     */
    stop(): boolean;

    /**
     * Subscribe to the lifecycle event for this capture. Currently
     * only `"stopped"` is defined; other event names install
     * listeners that will simply never fire. Listeners are stored
     * on the handle object directly — they're freed when the handle
     * is garbage-collected, so there's no leak from forgetting to
     * `removeEventListener`.
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
 * One match returned from `pane.findText`. Anchored on logical-line ids;
 * `startCol` / `endCol` are cumulative cell offsets within the logical line,
 * NOT visual-row columns — they're directly usable as `addDecoration`
 * `startCol` / `endCol` (`Decoration::startCellOffset` /
 * `endCellOffset`). `endCol` is exclusive (one past the last matched cell).
 *
 * `startRowId === endRowId` for every match — the search engine confines
 * each hit to a single logical line.
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
 * Common base for everything backed by a live terminal emulator:
 * `MbPane`, `MbPopup`, `MbEmbeddedTerminal`. Lets applets write generic
 * helpers that take "any terminal-like" without branching on kind.
 *
 * The global `Terminal` exposes this class — `x instanceof Terminal` is
 * true for every subclass instance.
 */
interface MbTerminal {
    /** Viewport width in character cells. */
    readonly cols: number;
    /** Viewport height in character cells. */
    readonly rows: number;
    /**
     * Current cursor state. `rowId` is a stable logical-line id (same
     * numbering as `pane.oldestRowId`/`newestRowId`). On a pane this is
     * gated on `pane.read`; on popups/embeddeds it's ungated — applets
     * can always introspect their own children.
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
     * scroll, autowrap reflow, and scrollback eviction (until the anchor
     * line evicts past the archive cap). Returns an opaque id usable with
     * `removeDecoration`. Requires `pane.read`.
     *
     * Composition order against other overlays: User decorations paint on
     * top of cell SGR, below hyperlink underlines, the OSC 133 command
     * region, and selection. fg / bg / strikethrough are last-writer-wins
     * within User decorations (insertion order, broken by `zPriority`).
     * Underline is first-writer-wins and never overrides a cell's own SGR
     * underline.
     *
     * `tag` groups User decorations for `clearDecorations(tag)`. The
     * reserved tags `"selection"`, `"command-region"`, `"hyperlink"` are
     * rejected (system kinds own them).
     *
     * `style.outline` and `style.dimOthers` are intentionally not exposed:
     * those are uniform-driven render primitives currently dedicated to
     * the OSC 133 command region.
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
        };
        tag?: string;
        zPriority?: number;
        shape?: "range" | "rectangle";
    }): number;

    /** Remove a decoration by id. Returns true iff it existed. */
    removeDecoration(id: number): boolean;

    /**
     * Remove User decorations. With no arg or empty tag, clears every
     * User decoration; system kinds (selection / command-region /
     * hyperlink) are never touched. Returns the count cleared.
     */
    clearDecorations(tag?: string): number;
}

declare const Terminal: MbTerminal;

interface MbPane extends MbTerminal {
    /**
     * Stable UUID of this pane's Terminal node in `mb.layout`. Use this with
     * `mb.layout.splitPane(...)`, `focusPane(...)`, `removeNode(...)`,
     * `killTerminal(...)`, and `node(...)`. Ungated — UUIDs are just
     * handles; mutations through `mb.layout` carry their own permission
     * discipline.
     */
    readonly nodeId: string;
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
     * `pane.read` permission not granted.
     * Start is always before or equal to end (normalized).
     * Column values are exclusive (one past the last selected column),
     * matching `getTextFromRows` convention. Requires `pane.read`.
     */
    readonly selection: {
        readonly startRowId: number;
        readonly startCol: number;
        readonly endRowId: number;
        /** Exclusive — one past the last selected column. */
        readonly endCol: number;
    } | null;
    // `cursor` is inherited from MbTerminal. On a pane specifically,
    // reading it still requires `pane.read` at runtime.

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
     * `endCol` is exclusive (one past the last column). Requires `pane.read`.
     */
    getTextFromRows(startRowId: number, startCol: number, endRowId: number, endCol: number): string;
    /**
     * Write the same text `getTextFromRows` would return directly to a
     * file. Useful for dumping selections, command output, or arbitrary
     * scrollback ranges without materialising the result as a JS string
     * the script then has to forward through `mb:fs`.
     *
     * Atomic on success: writes to `<path>.tmp` first, then renames into
     * place. The destination directory is auto-created. The path is
     * validated against the same sandbox as `fs.writeFileSync` —
     * built-in scripts may write anywhere; user scripts may write only
     * under `<configDir>/<scriptStem>/`.
     *
     * Range semantics are identical to `getTextFromRows`: `startRowId`
     * resolves to the first abs row of that logical line, `endRowId` to
     * the last; `startCol` is inclusive, `endCol` exclusive. An empty
     * range writes a zero-byte file (does not throw).
     *
     * Requires both `pane.read` (the source) and `fs.write` (the
     * destination). Returns the number of bytes written. Throws on
     * permission denial, sandbox violation, or I/O failure.
     */
    writeRangeToFile(path: string, startRowId: number, startCol: number, endRowId: number, endCol: number): number;
    /**
     * Open a streaming capture of this pane's PTY output. Returns a
     * handle the script can later `.stop()`; multiple concurrent
     * captures per pane are supported as long as their paths are
     * distinct.
     *
     * Captured bytes are taken at the post-coalesce / pre-filter
     * point inside the emulator — they reflect what the terminal
     * actually saw on the wire, regardless of any script-level
     * `output` filter that's also attached to this pane.
     *
     * Path is validated against the same sandbox as
     * `fs.writeFileSync` (built-in scripts unrestricted; user
     * scripts confined to `<configDir>/<scriptStem>/` or
     * `/tmp/masterbandit/`). The destination directory is auto-
     * created. Existing files are truncated.
     *
     * If a write fails (disk full, EIO, ...), the capture
     * auto-stops and the handle's `stopped` event fires with
     * `reason="io-error"` and the strerror text. Other captures on
     * the same pane are unaffected.
     *
     * Captures are tracked on the calling instance — unloading the
     * script auto-stops every capture it owns.
     *
     * Requires both `pane.read` (the source) and `fs.write` (the
     * destination). Throws on permission denial, sandbox violation,
     * duplicate path on the same pane, or open failure.
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
     * Search the pane's scrollback + visible grid for `needle`. Each match
     * is confined to a single logical line (`startRowId === endRowId`).
     * Cell columns are cumulative offsets within the logical line, suitable
     * for use as `addDecoration` `startCol` / `endCol`.
     *
     * Default behavior is case-insensitive literal substring search. Set
     * `opts.regex = true` to interpret `needle` as an ECMAScript regex
     * (invalid syntax returns an empty array rather than throwing). Set
     * `opts.caseSensitive` to disable folding. Set `opts.wholeWord` to
     * require word boundaries (literal-mode only — regex callers can use
     * `\b` directly). `opts.limit` caps the number of matches returned;
     * the walk stops early when reached.
     *
     * Walks oldest → newest, so the returned vector is sorted in scroll
     * order (top of scrollback first). Empty `needle` returns `[]`.
     *
     * Lines that straddle the scrollback / visible-grid boundary (a
     * partially-soft-wrapped live edit line) are searched on the
     * scrollback side only — the visible-grid continuation can't be
     * decoration-anchored correctly via line-id resolution.
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
     * Scroll the viewport so the line with the given logical id is at the
     * top. Returns `true` iff the viewport actually changed; `false` if the
     * id was evicted from the archive cap, the row is already in the live
     * viewport, or the row is already at the visible top. Requires
     * `pane.read`.
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
     * wrapping — use this for synthetic keystrokes, OSC responses, or any
     * payload that isn't semantically a user paste. Requires `shell.write`.
     * Throws if no PTY.
     */
    write(data: string): void;
    /**
     * Paste text to the PTY master. When the terminal has DECSET 2004
     * (bracketed paste mode) active, `data` is wrapped in `\x1b[200~` /
     * `\x1b[201~` so the shell's paste handling (quoting, auto-suggest
     * suppression, multiline confirmation, etc.) kicks in. When mode 2004
     * is inactive, behaves like `write()`. Requires `shell.write`. Throws
     * if no PTY.
     */
    paste(data: string): void;
    /** Create a popup on this pane. Requires `ui.popup.create`. Returns null on failure. */
    createPopup(opts: { id: string; x: number; y: number; w: number; h: number }): MbPopup | null;

    /**
     * Create an inline embedded terminal anchored at the current cursor
     * row. The returned object is a full `MbTerminal`: inject bytes,
     * resize, listen for user keystrokes when focused, listen for
     * destruction (either explicit `close()` or eviction once the anchor
     * row falls off the archive cap). `cols` matches this pane's cols;
     * `rows` is the caller-specified height. Returns `null` while the
     * pane is on alt-screen, when `rows <= 0`, or when an embedded
     * already exists on the current cursor row. Requires
     * `ui.popup.create` (embeddeds share the popup authority bit).
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
     * Fires when a logical-line id is evicted from this pane's
     * scrollback past the archive cap. Payload carries the evicted
     * `rowId` — after this fires, the id is no longer valid for any
     * text/link/selection query on this pane (calls will return empty
     * results). Useful for scripts that anchor state to specific rows
     * (prompt markers, bookmarks, embedded terminals) and need to
     * invalidate their tracking. Requires `pane.read`.
     *
     * Subscribing or removing a listener requires `pane.read`. The
     * event fires once per evicted id, on the main thread, after the
     * parser has yielded its lock — handlers can safely call any
     * synchronous pane API.
     */
    addEventListener(event: "rowEvicted", fn: (ev: { rowId: number }) => void): void;

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
 * Headless child terminal anchored to a Document line id in its parent pane.
 * Renders inline at the anchor row; Model B displacement shifts subsequent
 * rows down by `(rows - 1) * cellH` at composite time. Hidden while the
 * parent is on alt-screen; auto-destroyed when the anchor line evicts past
 * the archive cap (fires the `"destroyed"` event).
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
     * Fires after a successful `resize(rows)`. Payload: (cols, rows). Cols
     * is the parent pane's width at the time — currently doesn't fire when
     * the parent pane cols change (that cascade isn't wired).
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
 * the `id` field is only present on `"loaded"`, and `error` only on `"error"`.
 */
type MbLoadResult =
    | { status: "loaded"; id: number }
    | { status: "pending" }
    | { status: "denied" }
    | { status: "error"; error?: string };

// ============================================================================
// Config snapshot — mirrors the C++ Config struct via glaze JSON
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
    colors: MbConfigTabBarColors;
}

interface MbConfigNotifications { show_when_foreground: boolean; }

/**
 * One entry in `config.keybinding`. The TOML / glaze key is the singular
 * `keybinding` (matches `[[keybinding]]` table-array syntax in TOML).
 */
interface MbKeybinding {
    /** A single key like `"ctrl+shift+t"` or a sequence like
     *  `["ctrl+x", "ctrl+c"]` for kitty-style multi-stroke bindings. */
    keys: string[];
    /** Snake-case action name, optionally namespaced (`"namespace.action"`)
     *  for script-registered actions. */
    action: string;
    /** Action-specific positional arguments (e.g. `["right"]` for split_pane). */
    args?: string[];
}

/**
 * One entry in `config.mousebinding`. The TOML / glaze key is the singular
 * `mousebinding`.
 */
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
 * Live snapshot of the parsed TOML Config. Read via `mb.config`.
 *
 * The snapshot also carries mutation methods at runtime (see
 * {@link MbConfigMutations}), gated on the `config.modify` permission.
 * Methods are attached to the object returned by the `mb.config` getter
 * each time it is read, so capturing into a local variable retains them:
 * `const cfg = mb.config; cfg.patch({...});`.
 */
interface MbConfig extends MbConfigMutations {
    font: string;
    font_size: number;
    bold_strength: number;
    /**
     * Currently startup-only: changes to this field via TOML hot-reload
     * or `mb.config.patch` update the snapshot but do NOT resize existing
     * panes' scrollback rings (and new panes spawned post-reload still
     * use the startup value). This is a known limitation.
     */
    scrollback_lines: number;
    padding: MbConfigPadding;
    cursor: MbConfigCursor;
    colors: MbConfigColors;
    tab_bar: MbConfigTabBar;
    /** Glaze serialises the keybindings vector under the singular key. */
    keybinding: MbKeybinding[];
    mousebinding: MbMousebinding[];
    divider_color: string;
    divider_width: number;
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
    /** Reported color preference for mode 2031 / DSR-997. `"auto"` (default)
     *  defers to the system; `"light"` / `"dark"` overrides bypass DBus
     *  entirely (useful when the freedesktop portal is missing). */
    color_scheme: "auto" | "light" | "dark";
    /** Whether `closePane` / `closeTab` / OS window-close prompts before
     *  killing the focused unit. `"never"` always closes immediately;
     *  `"if_busy"` (default) prompts when any non-shell foreground process
     *  is running; `"always"` prompts unconditionally. The "shell" list is
     *  JS-side state in default-ui.js; mutate it via the
     *  `default-ui.add-shell` / `default-ui.remove-shell` actions. */
    confirm_close: "never" | "if_busy" | "always";
    notifications: MbConfigNotifications;
}

/**
 * Runtime config mutators (gated on the `config.modify` permission).
 * All mutations go through the same `applyConfig` path as TOML hot-reload
 * and are ephemeral — last-write-wins against a concurrent disk edit, and
 * nothing is persisted back to `config.toml`. Throws an `Error` on
 * validation failure (bad shape / wrong types).
 */
interface MbConfigMutations {
    /**
     * Deep-merge a partial config into the live snapshot. Plain-object
     * fields recurse; arrays (`keybinding`, `mousebinding`) and primitives
     * are replaced wholesale, so to add a single keybinding via patch
     * the caller must supply the full new array. {@link addKeybinding} is
     * a more ergonomic shortcut for that case.
     *
     * @example
     *   mb.config.patch({ font_size: 14, tab_bar: { position: "top" } });
     */
    patch(partial: Partial<MbConfig>): void;

    /**
     * Append a single keybinding to `config.keybinding[]` and apply.
     * Convenience over `patch({keybinding: [...mb.config.keybinding, b]})`.
     * Does not deduplicate — later entries win in the merge inside
     * `applyConfig` (which always layers `config.keybinding[]` over the
     * built-in `defaultBindings()`).
     */
    addKeybinding(b: MbKeybinding): void;

    /**
     * Remove all entries whose `keys` array exactly matches (element-wise,
     * order-sensitive). Returns the count removed (0 if none).
     * Cannot remove built-in default bindings — those are layered in by
     * `defaultBindings()` and not part of `config.keybinding[]`.
     */
    removeKeybinding(match: { keys: string[] }): number;

    /** Append a single mousebinding to `config.mousebinding[]` and apply. */
    addMousebinding(b: MbMousebinding): void;

    /**
     * Remove mousebindings whose specified fields match. Omitted fields
     * are wildcards — e.g. `removeMousebinding({button: "middle"})`
     * removes every middle-button binding regardless of event/mode/region.
     * Returns the count removed.
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
 * The array returned by `mb.actions` is augmented with handler-registry
 * methods. Capturing `const a = mb.actions; a.register(...)` works because the
 * methods are bound to the snapshot. The array itself is regenerated on every
 * `mb.actions` access.
 */
interface MbActions extends ReadonlyArray<MbActionInfo> {
    /**
     * Register a JS handler invoked when the action fires. Requires
     * `layout.modify` (used by built-in scripts that own action plumbing).
     */
    register(name: string, fn: (...args: string[]) => void): void;
    /** Drop a previously-registered handler. Requires `layout.modify`. */
    unregister(name: string): void;
}

/**
 * Payload for the `terminalExited` event on the `mb` global. Emitted after a
 * Terminal node's PTY child exits. The Terminal node remains in the layout
 * tree so the controller can decide whether to remove the node, transform the
 * tab, or spawn a replacement in response.
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
     * Live snapshot of the loaded TOML config (the same struct
     * `PlatformDawn::applyConfig` consumes). Re-read after the
     * `configChanged` event to pick up hot-reload updates. Shape mirrors
     * the C++ `Config` struct via glaze JSON serialization.
     *
     * The returned object is freshly built on every getter access (it is
     * NOT live-bound — mutating individual fields on the returned object
     * has no effect). To change config from JS, use the mutation methods
     * attached to the returned object (`patch` / `addKeybinding` / etc.,
     * see {@link MbConfigMutations}); these go through the same
     * `applyConfig` path as a TOML hot-reload.
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
     * Generate `length` cryptographically-secure random bytes and return them
     * as a `2 * length`-character hex string. Defaults to 32 bytes (64 hex
     * chars). Ungated — no permission required.
     */
    createSecureToken(length?: number): string;
    /**
     * Generate a random UUID v4 (36-char canonical string form). Ungated —
     * randomness alone confers no capability. String form is the only
     * JS-safe representation; 128-bit integers don't round-trip through
     * JS Number.
     */
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

    // --- External process management ---
    /**
     * External (non-PTY) process APIs. Distinct from `pane.write` /
     * `pane.paste` (which talk to an existing pane's shell) and from
     * `mb.layout.createTerminal` (which spawns a new pane with a PTY).
     * Use this for editors, GUI tools, OS integrations, etc. — anything
     * that should run alongside mb rather than inside a terminal pane.
     */
    readonly process: MbProcess;

    // --- Lifecycle events ---
    /** Fires once per new pane. Fires on every loaded instance. */
    addEventListener(event: "paneCreated", fn: (pane: MbPane) => void): void;
    /**
     * Fires after a pane has been destroyed. The pane handle is no longer
     * usable, so the payload is the scalar `(paneId, paneNodeId)` pair.
     * `paneNodeId` is `null` if the pane had already detached from the tree.
     */
    addEventListener(
        event: "paneDestroyed",
        fn: (paneId: string, paneNodeId: string | null) => void
    ): void;
    /** Fires once per new tab. Payload is the new tab's subtreeRoot UUID. */
    addEventListener(event: "tabCreated", fn: (tabNodeId: string) => void): void;
    /** Fires after a tab has been destroyed. Payload is the closed tab's UUID. */
    addEventListener(event: "tabDestroyed", fn: (tabNodeId: string) => void): void;
    /**
     * Fires when a Terminal node's child process exits. The Terminal node is
     * left in place so the controller can decide what to do (remove node,
     * transform the tab, respawn).
     */
    addEventListener(event: "terminalExited", fn: (ev: MbTerminalExitedEvent) => void): void;
    /** Fires when the persisted config has been reloaded from disk. */
    addEventListener(event: "configChanged", fn: () => void): void;
    /**
     * OS-level window close (X button / NSApp termination / Cmd+Q where
     * applicable). When at least one listener is registered, C++ defers the
     * quit and fans out the event; the listener owns the quit decision and
     * must call `mb.quit()` to commit. With no listener registered the
     * fallback path quits immediately, so a broken JS engine can't trap the
     * user. default-ui.js listens by default to drive close-confirm.
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
    removeEventListener(event: "tabCreated", fn: (tabNodeId: string) => void): void;
    removeEventListener(event: "tabDestroyed", fn: (tabNodeId: string) => void): void;
    removeEventListener(event: "terminalExited", fn: (ev: MbTerminalExitedEvent) => void): void;
    removeEventListener(event: "configChanged", fn: () => void): void;
    removeEventListener(event: "action", fn: (actionName: string) => void): void;
    removeEventListener(event: "action", actionName: string, fn: (...args: string[]) => void): void;
    removeEventListener(
        event: "scriptPermissionRequired",
        fn: (path: string, permissions: string, hash: string) => void
    ): void;
}

// ============================================================================
// mb.layout — LayoutTree primitives
// ============================================================================

/**
 * Per-child slot constraints inside a Container. All cell counts are 0 when
 * unset; defaults match the C++ `ChildSlot` struct.
 */
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
    /** The enclosing tab's subtreeRoot UUID, or `null` if not in a tab. */
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
     * Create an empty tab. Returns the new tab's subtreeRoot UUID string, or
     * `null` on failure.
     */
    createTab(): string | null;
    /**
     * Close a tab by its subtreeRoot UUID. Returns `false` when no tab
     * matched the argument (e.g. nil UUID, garbage, or last-tab guard).
     */
    closeTab(nodeId: string): boolean;
    /** Activate a tab by its subtreeRoot UUID. */
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

    // --- Tree mutation primitives ---

    /** Promote a free-floating subtree to root. */
    setRoot(nodeId: string): void;
    /** Returns the root node id, or `null` if there is none. */
    getRoot(): string | null;
    /** Append `child` under `parent` with optional slot constraints. */
    appendChild(parent: string, child: string, opts?: MbChildSlotOptions): void;
    removeChild(parent: string, child: string): void;
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
     * Enumerate every node of `kind` reachable from `subtreeRoot` (or from the
     * tree root if omitted). Recursion follows Container and Stack children;
     * TabBar has no children. Returned UUIDs are in implementation-defined
     * tree-walk order.
     */
    queryNodes(
        kind: "Terminal" | "Container" | "Stack" | "TabBar"
            | "terminal" | "container" | "stack" | "tabbar",
        subtreeRoot?: string | null
    ): string[];
    /**
     * Find the first node whose `label` exactly equals `label`. Returns
     * `null` if none. Empty strings never match (unlabeled nodes are
     * not findable).
     */
    findByLabel(label: string): string | null;
    /**
     * Activate a child of the Stack bound to `barUuid`. The second argument
     * may be a positional index into the bound Stack's `children` array, or
     * the UUID of one of those children. Returns `true` on success, `false`
     * if the index is out of range or the child UUID is not in the Stack.
     * Throws if `barUuid` is not a TabBar or its `boundStack` is nil.
     *
     * Distinct from `activateTab`, which targets the global "active tab"
     * (root-Stack activeChild). Use this for per-bar activation in layouts
     * with more than one TabBar.
     */
    activateTabInBar(barUuid: string, indexOrChildUuid: number | string): boolean;
    /**
     * Compute pixel rects for every node, given the window rect and per-cell
     * pixel dimensions. Result is a UUID-keyed map.
     */
    computeRects(window: MbRect, cellW?: number, cellH?: number): { [nodeId: string]: MbRect };
    /** Snapshot of the focused pane and its enclosing tab, or `null`. */
    focusedPane(): MbFocusedPaneInfo | null;
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
