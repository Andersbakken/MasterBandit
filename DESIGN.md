# MasterBandit Terminal — Design

## 1. Text Rendering (implemented)

GPU-accelerated glyph rendering via hb-gpu / Slug. Glyph outlines are encoded
CPU-side into a storage buffer atlas; the GPU fragment shader rasterizes directly
from Bézier curve data — no bitmap textures.

**Atlas**: `array<vec4<i32>>` storage buffer, 1M texels (4 MB) pre-allocated per
font. Each texel holds 4 int32 values widened from the hb-gpu int16-pair encoding.
Atlas offset 0 is reserved as a sentinel (empty glyph); `atlasUsed` starts at 1.

**Glyph encoding**: Lazy. ASCII 32–126 pre-warmed at font registration; remaining
glyphs encoded on demand during shaping. Empty glyphs (whitespace) store
`atlas_offset = 0`.

**Per-vertex tint**: Packed RGBA8 `uint32_t` (R in low byte). Unpacked in the
fragment shader to `vec4<f32>`. Enables per-glyph coloring.

**Rect pipeline**: Separate shader, vertex buffer, and pipeline for
cursor/selection/underlines/dividers. Rects render before text so glyphs overlay.

**Render order**: clear → rect pass → text pass (per font).

Terminal data model uses a `cols × rows` cell grid (§2) with per-cell codepoint
and packed attributes. A compute shader (§5) generates vertices from resolved
cells.

---

## 2. Cell Grid (implemented)

A `cols × rows` cell array providing O(1) random access for cursor-addressed writes.

| Field | Size | Contents |
|---|---|---|
| `char32_t wc` | 4 B | Unicode codepoint |
| packed attrs | 8 B | 24-bit fg, 24-bit bg, 2-bit color mode per color, 1-bit flags: bold, italic, underline, strikethrough, blink, inverse, dim |

Rare attributes (hyperlinks, combining characters, image refs) are stored in a
lazy-allocated side table keyed by cell coordinate.

**Wide characters**: first cell holds the glyph, subsequent cells are marked as
spacers (same model reused for ligatures).

**SGR inverse**: fg/bg swap applied at cell-resolution time in `resolveRow`, not
stored as separate state. Enables correct rendering of TUI cursors and selections.

---

## 3. Scrollback (implemented)

Two-tier design.

### Tier 1 — Ring buffer of full cell rows

- Visible screen + bounded history (configurable, default infinite).
- Each row is a contiguous array of cells.
- Mutable; direct target of terminal writes.

### Tier 2 — Compressed archive

- Lines re-encoded as UTF-8 text + inline ANSI SGR escape sequences.
- Read-only. Re-parsed on demand when the user scrolls back far enough.
- Supports unlimited scrollback without proportional memory cost.

Tier 1 overflow pushes the oldest row into Tier 2 before reuse.

---

## 4. Run-Based Shaping & Ligatures (implemented)

**Principle**: the cell grid stores raw codepoints and attributes only — no
ligature or shaping state is persisted. Shaping happens at render time via
`TextSystem::shapeRun()`.

**Run building** (CPU, per dirty row in `resolveRow()`):

1. Accumulate runs of adjacent cells sharing font-affecting attributes
   (bold, italic). Runs do NOT break on color — this allows ligatures across
   syntax highlighting boundaries.
2. Feed each run to `shapeRun()` which performs script-run detection (SheenBidi),
   per-segment HarfBuzz shaping, and font fallback.
3. Map shaped glyphs back to cells via HarfBuzz cluster indices.

**Glyph positioning**: normal (non-substituted) glyphs anchor at their cell's
grid origin. Substituted glyphs (ligatures, Arabic contextual forms) use
HarfBuzz advance-based positioning to preserve inter-glyph relationships.
Substitution is detected by comparing shaped glyph ID against nominal lookup.

**RTL support**: SheenBidi BiDi analysis determines per-character embedding
levels. RTL segments are shaped with `HB_DIRECTION_RTL`. Cell assignments for
RTL glyphs are mirrored within their contiguous RTL range.

**Indirect glyph list**: `ResolvedCell` stores `glyph_offset` + `glyph_count`
into a separate `GlyphEntry` storage buffer. Each `GlyphEntry` carries atlas
data, font extents, and per-glyph x/y offsets from HarfBuzz. The compute shader
loops over each cell's glyphs, supporting combining marks (multiple glyphs per
cell) and ligatures (zero glyphs in trailing cells).

**Per-row cache**: shaped run results are cached per row in `PaneRenderState`.
Only dirty rows are re-shaped. Clean rows reuse cached glyph data.

---

## 5. GPU Pipeline (implemented)

Per-frame flow:

1. **CPU**: shape dirty rows → produce a resolved cell buffer. Each entry contains
   glyph ID, atlas offset, cell span, and packed color. SGR inverse applied here.
2. **Upload**: resolved cells written to a `ComputeState` GPU storage buffer
   (acquired from `ComputeStatePool`).
3. **Compute shader**: reads resolved cells + font metrics + cursor UBO, emits
   `SlugVertex` entries into a vertex buffer. Also emits `RectVertex` for
   non-default backgrounds and cursor geometry (solid or hollow).
4. **Render passes**: rect pass (backgrounds, cursor, selection) then Slug text
   pass. Viewport is set to the full pane dimensions (including padding) so
   NDC mapping accounts for padding offsets in glyph positioning.
5. **Texture pool**: rendered output goes into a `PooledTexture` from
   `TexturePool`. Textures are held between frames when content is unchanged and
   re-acquired when dirty. Both pools use byte-budget LRU eviction.
6. **Compositor**: composites all visible pane textures plus the tab bar texture
   onto the swapchain via `CopyTextureToTexture`. An explicit viewport is set on
   each render pass to ensure correct NDC mapping when a pool texture is larger
   than the content area.
7. **Dividers**: after compositor copies, each pane that owns a split boundary
   draws its divider rect via a persistent 6-vertex GPU buffer through the rect
   pipeline. No per-frame allocation.

---

## 6. Dirty Tracking (implemented)

- Per-row dirty bit in the grid.
- Per-pane `dirty` flag in `PaneRenderState` — set on terminal update events.
- Only dirty panes acquire a new pool texture and re-render.
- Clean panes reuse their held texture and are composited without re-rendering.
- Tab bar has its own `tabBarDirty_` flag.
- Focus changes mark both the previously-focused and newly-focused pane dirty
  so cursor type (solid/hollow) updates immediately.
- Inactive tab textures are released to the pool on tab switch; re-rendered on
  return.

---

## 7. Multi-Tab / Multi-Pane Architecture (implemented)

### Hierarchy

```
PlatformDawn
  └── tabs_: Vec<Tab>            ← tab bar shows one entry per Tab
        └── Tab
              ├── layout_: Layout      ← binary split tree of Panes
              │     └── Pane           ← one terminal per pane
              │           ├── terminal_: Terminal  (PTY + VT parser)
              │           ├── title_: string       (per-pane OSC title)
              │           └── popups_: Vec<PopupPane>  (OSC 58237)
              └── overlays_: Vec<Terminal>  ← full-screen overlays (no tab bar entry)
```

### TerminalEmulator / Terminal split

- **`TerminalEmulator`**: VT parser + cell state + selection + image registry.
  No PTY. Can be used standalone for popup panes. Callbacks via `TerminalCallbacks`.
- **`Terminal`** extends `TerminalEmulator`: adds PTY management (fork, masterFD,
  `readFromFD`, resize+ioctl). PTY exit calls `PlatformCallbacks::onTerminalExited`
  which closes the pane (or tab if last pane, or quits if last tab).

### Tab bar

- Rendered as a 1-row cell grid using the same compute/text/rect GPU pipeline.
- Powerline style: U+E0B0 chevron separators via bundled Symbols Nerd Font Mono.
- Per-tab: index `[N]`, optional icon (OSC 1), title from focused pane (OSC 2),
  progress glyph (OSC 9;4) with indeterminate animation.
- Configurable: style (powerline/hidden), position (top/bottom), font, font size,
  colors. Stored in `[tab_bar]` config section.
- Left-click: switch tab. Middle-click: close tab (no-op if last tab).

### Pane titles

Each `Pane` stores its own `title_` and `icon_` set by OSC sequences from its
shell. Title changes propagate to the tab bar only from the focused pane. On pane
focus change, the tab bar and window title update immediately from the newly
focused pane's stored title.

### Pane focus and focus events

`Layout::focusedPaneId()` tracks the active pane. On focus change:
- Focus-out sequence (`CSI O`, mode 1004) sent to the outgoing pane's terminal.
- Focus-in sequence (`CSI I`) sent to the incoming pane's terminal.
- Both panes marked dirty so cursor type (solid/hollow) updates immediately.

### Cursor rendering

Cursor position, type, and color are passed as fields in `TerminalComputeParams`
(the compute shader UBO) rather than modifying cell data:

- `cursor_type = 0`: no cursor (viewport scrolled, cursor hidden).
- `cursor_type = 1`: solid filled rect — focused pane.
- `cursor_type = 2`: hollow outline (4 thin rects) — unfocused pane.

Cursor geometry is emitted directly by the compute shader. The rect vertex buffer
has 24 extra slots beyond `cells × 6` to accommodate hollow cursor geometry.

### Pane dividers

Each split node in the layout tree has a 1–N px gap between children. The "first"
(left/top) child in each split owns a persistent 6-vertex GPU buffer for its
adjacent divider rect. Drawn via the rect pipeline after the compositor pass.
Divider buffers are rebuilt on layout changes and cleared on tab switch / resize.
Color (`divider_color`) and width (`divider_width`) are configurable.

### Popup panes (OSC 58237)

Shell-driven floating cell grids. No PTY — content written via escape sequence.
Protocol: `OSC 58237 ; create/write/focus/blur/destroy ; id=<id> ; ... ST`.
Keyboard routing: focused popup receives keys; responses go back via owning
terminal's PTY.

### Resource pools

- **`TexturePool`**: GPU textures (RenderAttachment|CopySrc). Best-fit acquire,
  immediate release, byte-budget LRU eviction.
- **`ComputeStatePool`**: sets of 5 compute buffers + bind group. Same
  acquire/release/eviction pattern. Lives inside `Renderer`.

---

## 8. Kitty Keyboard Protocol (implemented)

Progressive enhancement via flag stack. Apps push flags to enable features,
pop on exit. Each screen (main/alt) has an independent 8-entry stack.

**Flags** (bitfield):
- `0x01` DISAMBIGUATE — functional keys use CSI u; text keys with modifiers use CSI u
- `0x02` REPORT_EVENT_TYPES — report release/repeat events via `:event` suffix
- `0x08` REPORT_ALL_KEYS — all keys use CSI u, including Enter/Tab/Backspace
- `0x10` REPORT_TEXT — embed text codepoints in CSI u sequence

**CSI dispatch**:
- `CSI > flags u` — push flags
- `CSI < count u` — pop N entries
- `CSI = flags ; mode u` — set flags (replace/OR/AND-NOT)
- `CSI ? u` — query current flags

**Key encoding**: functional keys map to Kitty PUA codes (57344+). Legacy keys
(arrows, F1-F4, tilde keys) use traditional CSI forms with modifier parameters.
Modifier-only keys reported only with REPORT_ALL_KEYS.

**RIS**: full reset clears both stacks and all terminal modes (cursor visibility,
shape, mouse modes, bracketed paste, kitty flags).

---

## 9. Reflow on Resize (implemented)

When the terminal width changes, soft-wrapped lines are re-wrapped to the new
width. Hard newlines (explicit `\n`) are preserved.

**Soft-wrap tracking**: each row in the Document ring buffer has a `continued`
flag (`std::vector<bool>` indexed by physical slot). Set by
`advanceCursorToNewLine()` when autowrap fires on the main screen. Explicit
line feeds do not set it. The flag is serialized in tier-2 `ArchivedRow`.

**Reflow algorithm** (Document::resize, column change):

1. Collect all source rows in order: tier-2 archive → tier-1 history → screen.
   Trailing blank screen rows below the cursor are trimmed.
2. Join logical lines: consecutive rows where all but the last have
   `continued=true`. Trailing blank cells in each source row are trimmed.
3. Re-wrap logical lines at the new width. Wide characters at the boundary get
   a padding space. Wide spacer cells are regenerated.
4. Cursor position tracked during the copy via `CursorTrack` (source absolute
   row+col → destination absolute row+col).
5. Result installed as new ring. Excess history evicted to tier-2 archive.

**Cursor beyond content**: if the cursor was at/past the last content line
(e.g. at a prompt with rprompt), the cursor is placed at the end of content
after reflow, not where reflow tracked it. This prevents rprompt wrapping
from shifting the cursor down.

**Alt screen**: does not reflow. CellGrid::resize truncates/pads as before.

**SIGWINCH coalescing**: `Terminal::resize()` sets a `mResizePending` flag.
`flushPendingResize()` sends a single `TIOCSWINSZ` ioctl per render frame,
after all resize events have been coalesced. Prevents rapid SIGWINCH storms
during window drag.

---

## 10. OSC 133 Shell Integration (implemented)

Prompt markers from the FinalTerm/semantic prompts protocol. Stored per-row
in the Document ring buffer (`promptKind_` vector).

**Markers**:
- `A` — prompt start (primary or secondary with `k=s`)
- `B` — command input starts (after prompt text)
- `C` — command output starts
- `D` — command finished (with exit code, currently ignored)

**Features using markers**:
- **Jump to prompt**: `scrollToPrompt(direction)` scans history + screen for
  `PromptStart` markers and scrolls the viewport. Keybindings: Cmd+Up/Down
  (macOS), Ctrl+Alt+Z/X (Linux).
- **Select command output**: `selectCommandOutput()` finds the `OutputStart`
  region around the viewport center and selects all text to the next
  `PromptStart`.
- **Prompt-aware reflow**: when OSC 133 markers are present, prompt lines can
  be blanked before reflow to prevent rprompt content from causing wrapping.

**Requires shell integration**: the shell must emit OSC 133 sequences. Kitty
auto-injects these via `ZDOTDIR` hijacking; we don't yet (planned).

---

## 11. Overlays (implemented)

Full-screen terminal overlays that cover the entire tab's layout area. Used
by the scrollback pager.

**Hierarchy**: `Tab::overlays_` is a stack of `unique_ptr<Terminal>`. When an
overlay is active, `activeTerm()` returns the top overlay for input routing.
The render loop detects `currentTab->hasOverlay()` and renders the overlay
terminal instead of the pane layout.

**Overlay render state**: stored in `overlayRenderStates_` keyed by `Tab*`.
Created on first render, cleaned up when the overlay exits. The overlay's
content area excludes the tab bar and resizes dynamically if the tab bar
changes visibility.

**Scrollback pager** (`ShowScrollback` action):
1. Serializes all scrollback (tier-2 archive + tier-1 history + screen) to a
   temp file via `serializeScrollback()`.
2. Spawns `less -R <tmpfile>` as an overlay terminal using `TerminalOptions::command`.
3. On exit (PTY closes), cleanup is deferred to the next idle tick to avoid
   use-after-free (the exit callback fires from `Terminal::readFromFD`).
4. Keybindings: Cmd+F (macOS), Ctrl+Shift+F (Linux).

**Mode 2026 (synchronized output)**: when active, rendering is deferred until
the app disables sync. Prevents showing intermediate states during screen
redraws. Checked per-pane in the render loop.

---

## 12. Keybindings / Actions (implemented)

Actions are defined as `Action::Any` (`std::variant` of all action structs) in
`Action.h`. Key bindings map sequences of `KeyStroke` (key + modifiers) to
actions using a `SequenceMatcher` state machine — same model as Kitty's `>` syntax.

**Config format** (`[[keybinding]]` in TOML):
```toml
[[keybinding]]
keys = ["ctrl+x", "2"]
action = "split_pane"
args = ["right"]
```

**Default bindings**:

| Keys | Action |
|---|---|
| Ctrl+Shift+T | new_tab |
| Ctrl+Shift+E | split_pane right |
| Ctrl+Shift+O | split_pane down |
| Ctrl+Shift+W | close_pane |
| Ctrl+Shift+Z | zoom_pane |
| Ctrl+Shift+Arrow | focus_pane (directional) |
| Ctrl+Shift+N/P | focus_pane next/prev |
| Meta+C / Meta+V | copy / paste |

Mouse bindings (tab bar click/middle-click) remain hardcoded pending a mouse
binding system.

---

## 13. Configuration (implemented)

TOML config at `$XDG_CONFIG_HOME/MasterBandit/config.toml`. Parsed via glaze.

```toml
font = "Inconsolata"
font_size = 20.0
bold_strength = 0.04
scrollback_lines = -1   # -1 = infinite
divider_color = "#3d3d3d"
divider_width = 1

[tab_bar]
style = "powerline"     # powerline | hidden
position = "bottom"     # top | bottom
font = ""               # empty = same as terminal font
font_size = 0.0         # 0 = same as terminal font_size

[tab_bar.colors]
background  = "#1a1b26"
active_bg   = "#7aa2f7"
active_fg   = "#1a1b26"
inactive_bg = "#24283b"
inactive_fg = "#565f89"

[[keybinding]]
keys = ["ctrl+shift+e"]
action = "split_pane"
args = ["right"]
```

Font resolution uses fontconfig (Linux) or CoreText (macOS) for family→file
lookup, with monospace-first fallback for missing glyphs.

Bold: uses a real bold font variant if found; otherwise synthesizes via
`hb_font_set_synthetic_bold`.

PTY environment: `$TERM=xterm-256color`, `$COLORTERM=truecolor`.

---

## 14. Font Fallback (implemented)

Two-pass fontconfig strategy (same as WezTerm):
- **Pass 1**: monospace fonts only (FC_MONO / FC_DUAL / FC_CHARCELL), using
  `FcFontList` (database order, no sort-scoring bias).
- **Pass 2**: any font if no monospace covers the codepoint.

Unicode whitespace codepoints are excluded from fallback (no visible glyph needed).

Bundled fallback: **Symbols Nerd Font Mono** (OFL licensed) copied alongside the
binary, loaded as a registered fallback for powerline glyphs (U+E0B0–E0B3) and
other symbols.

---

## 15. Debug IPC (implemented)

WebSocket server on a Unix domain socket (`/tmp/mb-<pid>.sock`). Accessed via
`mb --ctl <command>`.

| Command | Description |
|---|---|
| `screenshot [--format grid\|png]` | Capture terminal content |
| `key --text <text>` | Send text input |
| `key --key <name> [--mod <mod>]` | Send named key with modifier |
| `stats` | GPU pool and pane memory usage (JSON) |
| `logs` | Stream log messages (Ctrl+C to stop) |

Stats response includes texture pool (total/in-use/free/bytes), compute pool, and
per-tab/per-pane information (dimensions, held texture size, divider buffer).

---

## 16. Project Structure

```
src/
  main.cpp                  — entry point, config loading
  platform/                 — GPU rendering, windowing, event loop (OBJECT library)
    PlatformDawn.h/cpp      — class definition, init, createTerminal, factory
    EventLoop.cpp           — exec(), quit()
    Input.cpp               — GLFW key/mouse/resize callbacks
    Actions.cpp             — keybinding action dispatch
    Render.cpp              — resolveRow, renderFrame
    Tabs.cpp                — tab/pane lifecycle, PTY polling, dividers
    TabBar.cpp              — tab bar init and rendering
    Debug.cpp               — gridToJson, statsJson
    Renderer.h/cpp          — GPU pipeline setup, render passes, compositing
    ComputeStatePool.h/cpp  — GPU buffer pool for compute pass
    ComputeTypes.h          — ResolvedCell, GlyphEntry, TerminalComputeParams
    TexturePool.h/cpp       — GPU texture pool with LRU eviction
    InputTypes.h            — Key enum, KeyEvent, MouseEvent, modifiers
  terminal/                 — terminal emulation (OBJECT library)
    TerminalEmulator.h/cpp  — VT parser core: state machine, CSI, onAction
    KittyKeyboard.cpp       — kitty keyboard protocol + key encoding
    MouseAndSelection.cpp   — mouse event handling, text selection
    SGR.cpp                 — Select Graphic Rendition attribute parsing
    OSC.cpp                 — OSC processing, iTerm2 images, clipboard, notifications
    Terminal.h/cpp          — PTY management (fork, read, write, resize)
    Document.h/cpp          — scrollback: ring buffer + compressed archive
    CellGrid.h/cpp          — simple cols×rows cell array
    CellTypes.h             — Cell, CellAttrs, CellExtra structs
    IGrid.h                 — abstract grid interface
  text.h/cpp                — HarfBuzz shaping, font atlas, glyph cache, SheenBidi
  Layout.h/cpp              — binary split tree for pane layout
  Pane.h/cpp                — terminal container with popup support
  Tab.h/cpp                 — tab container with overlay support
  Bindings.h/cpp            — keybinding parsing and sequence matching
  Config.h/cpp              — TOML config loading
  ScriptEngine.h/cpp        — QuickJS-ng scripting engine, JS API surface
  ScriptPermissions.h/cpp   — permission bitmask, allowlist, hash verification
  ClickDetector.h/cpp       — multi-click detection for mouse bindings
assets/
  scripts/
    applet-loader.js        — built-in: OSC 58237 applet loading + permission prompts
    command-palette.js      — built-in: fuzzy command palette (Cmd+Shift+P)
examples/
  popup-demo.js             — example popup script
tests/
  TestTerminal.h            — lightweight TerminalEmulator wrapper for tests
  test_*.cpp                — doctest test files
```

---

## 17. Testing (implemented)

Unit test suite using doctest (`tests/`). `TestTerminal` wraps `TerminalEmulator`
with no PTY or GPU — feeds escape sequences via `injectData`, asserts on cell grid
state. 4096-line scrollback enabled by default. Tests link the `terminal` OBJECT
library directly.

Coverage: text output, cursor movement (CUP/CUU/CUD/CUF/CUB/CHA/CNL/CPL/VPA),
SGR (all attributes, 16/256/truecolor fg/bg, inverse, turn-off codes), screen
operations (ED/EL/SU/SD/DECSTBM/DCH/ICH/IL/DL/ECH), terminal modes (alt screen,
mouse 1000/1002/1003, bracketed paste, sync output, RIS, DA/XTVERSION), scrollback
viewport, OSC 0/1/2 title/icon, wide characters (CJK, emoji), SGR inverse
color swap semantics, kitty keyboard protocol (mode management, key encoding,
all flag modes, legacy compatibility, modifier keys, comprehensive RIS reset),
shapeRun() (ASCII, Arabic RTL, mixed LTR/RTL, cache, multibyte UTF-8).

Run with: `./build/bin/mb-tests`

---

## 18. Scripting Engine (implemented)

QuickJS-ng embedded scripting with permission system.

### Architecture

Built-in scripts loaded at startup via `Engine::loadController()` (fully trusted,
all permissions). User scripts loaded via `Engine::loadScript()` (permission-checked,
allowlist-backed). All scripts get the same `mb.*` API surface; unauthorized calls
throw and terminate the script.

### JS API surface

**`mb` global object (getter properties):**
- `mb.tabs` — array of Tab objects
- `mb.activePane` — focused Pane or undefined
- `mb.activeTab` — active Tab or undefined
- `mb.actions` — array of {name, label, builtin, args?}

**`mb` global object (functions):**
- `mb.invokeAction(name, ...args)` — execute an action
- `mb.addEventListener(event, fn)` — lifecycle/action events
- `mb.loadScript(path, permsStr)` — load user script (permission-checked)
- `mb.approveScript(path, response)` — approve/deny pending script (built-in only)
- `mb.createTab()` / `mb.closeTab(id?)` — tab management
- `mb.unloadScript(id)` — unload a script
- `mb.setNamespace(name)` — claim action namespace
- `mb.registerAction(name)` — register `namespace.action`
- `mb.exit()` — self-unload via deferred timer

**Pane object:**
- Properties: `id`, `cols`, `rows`, `title`, `cwd`, `hasPty`, `focused`,
  `focusedPopupId`, `foregroundProcess`, `popups`
- Methods: `addEventListener(event, fn)`, `inject(data)`, `write(data)`,
  `createPopup({id, x, y, w, h})`

**Popup object:**
- Properties: `paneId`, `id`, `focused`, `cols`, `rows`, `x`, `y`
- Methods: `addEventListener(event, fn)`, `inject(data)`, `resize({x,y,w,h})`,
  `close()`
- Events: `"input"` (keyboard when focused), `"mouse"` (click with cell coords)

**Tab object:**
- Properties: `id`, `panes`, `activePane`, `overlay`
- Methods: `addEventListener(event, fn)`, `createOverlay()`, `closeOverlay()`,
  `close()`

**Overlay object:**
- Properties: `cols`, `rows`, `hasPty`
- Methods: `addEventListener(event, fn)`, `inject(data)`, `write(data)`, `close()`

### Permission system

Granular bitmask: `ui` (overlay/popup create/destroy), `io` (filter input/output,
inject), `shell` (write to PTY), `actions` (invoke actions, register actions),
`tabs` (create/close), `scripts` (load/unload). Groups expand to individual bits.

Allowlist at `$XDG_CONFIG_HOME/MasterBandit/allowed-scripts.toml` with SHA-256
content hash and schema version. Hash change triggers re-prompt. Permission
violation terminates the script.

Confirmation prompt is JS-driven (applet-loader.js): popup with clickable
[allow]/[deny]/[always]/[never] buttons. Both keyboard and mouse input supported.

### Script actions

Scripts claim a namespace (`mb.setNamespace("palette")`), register actions
(`mb.registerAction("open")`), and listen via
`mb.addEventListener("action", "palette.open", fn)`. Config binds with
`action = "palette.open"`. Unknown dotted action names in config are parsed as
`ScriptAction` variants.

### Built-in scripts

- `applet-loader.js` — handles OSC 58237 applet loading and permission prompts
- `command-palette.js` — fuzzy command palette (Cmd+Shift+P)

### Mouse binding system

`MouseStroke` (button, event type, modifiers, mode, region) + `MouseBinding` +
`ClickDetector` for multi-click detection. Selection types: normal (drag),
word (doublepress), line (triplepress), extend (shift+click), rectangle
(alt+drag). Configurable via `[[mousebinding]]` in TOML. Default bindings
replace all hardcoded mouse logic.

### Foreground process tracking

`Terminal::foregroundProcess()` queries `tcgetpgrp()` on the PTY fd, resolves
via `proc_pidpath` (macOS) or `/proc/<pid>/comm` (Linux). Checked on each PTY
read; fires `foregroundProcessChanged` event on change. Tab title shows
foreground process name when no OSC 2 title is set.
