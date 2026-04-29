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
`sizeof(Cell) == 12` bytes, asserted statically.

| Field | Size | Contents |
|---|---|---|
| `char32_t wc` | 4 B | Unicode codepoint |
| `CellAttrs attrs` | 8 B | see below |

**`CellAttrs` layout** (`data[8]`, see `src/terminal/CellTypes.h`):

- `data[0..2]` — fg R,G,B (24-bit)
- `data[3..5]` — bg R,G,B (24-bit)
- `data[6]` bits:
  - bit 0 — fg color mode (0 = Default / use palette default, 1 = RGB). Indexed palette colors resolve to RGB at SGR parse time, so only these two modes are stored.
  - bit 1 — bg color mode (same)
  - bits 2–3 — OSC 133 semantic type (0 = Output, 1 = Input, 2 = Prompt). Orthogonal to SGR; `reset()` preserves it.
  - bits 4–7 — bold, italic, underline, strikethrough
- `data[7]` bits:
  - bits 0–3 — blink, inverse, dim, invisible
  - bits 4–5 — wide, wide-spacer
  - bits 6–7 — underline style (0 = straight, 1 = double, 2 = curly, 3 = dotted)

Rare attributes (`imageId`/`imagePlacementId`/`imageStartCol`/`imageOffsetRow`,
`hyperlinkId`, `underlineColor` for SGR 58, `combiningCp` for variation selectors
and combining marks) live in a lazy-allocated `CellExtra` side table keyed by
cell coordinate.

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

### Viewport anchoring

The viewport's top is anchored on a **logical line id + sub-pixel offset**
(`mTopLineId`, `mTopPixelSubY` on `TerminalEmulator`), not a row-count offset.
The legacy `viewportOffset()` accessor is derived from `mTopLineId` at read
time via `historySize - firstAbsOfLine(mTopLineId)`.

**Why.** Line ids are stable across scrollback migration / reflow / eviction,
so anchoring to one means:
- New content streaming in doesn't drift a scrolled-back viewport — the
  anchor's abs row is unchanged; `viewportOffset()` recomputes larger as
  `historySize` grows. (The old `mViewportOffset += n` compensation in
  `scrollUpInRegion` is gone.)
- Embedded resize (rows 2 → 10) doesn't move the top-of-viewport.
- Width-change reflow that renumbers physical rows leaves `mTopLineId`
  resolving to the same content.

**Live-mode auto-advance.** `scrollUpInRegion` captures whether the user was
at live (viewportOffset==0) before the scroll and re-points `mTopLineId`
to the newest top-of-screen row after, so live-tailing doesn't drift
backward one row per appended line.

**Sub-line scroll.** `scrollByPixels(int dyPx, int cellHeight)` advances
`mTopPixelSubY`; once the sub-offset crosses a cell boundary it rolls into
`mTopLineId`. The composite pass subtracts `topPixelSubY` from each pane
composite entry's `dstY` (with source-side cropping when it underflows the
pane rect), so smooth scroll moves content by its exact pixel delta. Wheel
ticks dispatch here with `dyPx = tick * scrollStepPx`; positive = into
history.

**Eviction.** If `mTopLineId`'s backing row evicts past the archive cap,
`firstAbsOfLine` returns -1 and `viewportOffset()` reports 0 (live). The
next `resetViewport` / `scrollViewport` / `scrollToPrompt` re-seeds the
anchor.

### Stable logical-line IDs

Every logical line (a soft-wrap chain terminated by a hard newline) gets a
unique monotonic 64-bit ID at write time, stored in a flat `std::deque<uint64_t>
rowLineId_` parallel to the abs-row space (`archive_ + tier-1 + screen`).

**Invariants:**
- Unique per logical line. Autowrap continuations inherit the predecessor's ID
  via `inheritLineIdFromAbove`, so multiple physical rows comprising one
  logical line share one ID.
- Monotonic non-decreasing across abs positions — enables O(log N) binary
  search via `firstAbsOfLine` / `lastAbsOfLine`.
- Survives scroll, tier-1 → tier-2 migration, width-change reflow, and
  height-only resize.
- Invalidated only when the line itself evicts past the archive cap.

**Reflow propagation.** During width-change reflow the rebuild carries each
source row's ID through via a `dstSrcIdx` vector (first-src-wins when multiple
sources merge into one dst row, with post-pass propagation of wrap-created
rows). Fresh mints are only used for the blank ring tail post-reflow, and
those mints are always strictly greater than any pre-reflow ID (from the
monotonic counter), preserving sortedness.

**Content extraction.** `Document::getTextFromLines(startLineId, endLineId,
startCol, endCol)` resolves via `firstAbsOfLine` / `lastAbsOfLine`, so a
multi-row wrapped line is covered end-to-end at the current width regardless
of how it was wrapped when the line ID was captured.

**JS exposure.** `CommandInfo`, `pane.cursor.rowId`,
`pane.selection.startRowId/endRowId`, `pane.oldestRowId/newestRowId`,
`pane.rowIdAt(row)`, `pane.getTextFromRows(...)`,
`pane.getLinksFromRows(...)` all expose these line-identifying IDs
(named `rowId` in the JS API for continuity) as stable handles safe to
hold across async boundaries.

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
- Powerline-style chevrons: U+E0B0 separators via bundled Symbols Nerd Font Mono.
- Per-tab: index `[N]`, optional icon (OSC 1), title from focused pane (OSC 2),
  progress glyph (OSC 9;4) with indeterminate animation.
- Configurable: visibility (`"auto"` = hide when only one tab, `"visible"` =
  always show, `"hidden"` = never show), position (`"top"` / `"bottom"`), font,
  font size, colors. Stored in `[tab_bar]` config section.
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

### Embedded terminals (document-anchored)

Headless sibling of popups, anchored to a stable logical-line id rather than
viewport cell coordinates, so they scroll with the document. Created from JS
via `pane.createEmbeddedTerminal({rows})`; stored in `Terminal::mEmbedded`
keyed on the anchor lineId. The anchor is captured from the parent's cursor
row at create time; the parent cursor is then advanced one row so subsequent
writes don't target the hidden anchor cell.

**Displacement model.** The parent's cell grid is unchanged — the embedded
only affects visual layout at composite time. `RenderEngine` splits the
parent's pane texture into strips around each visible anchor row: the anchor
row is skipped entirely (covered by the embedded), and every row below is
shifted down by `(embRows - 1) * cellH` (cumulatively across multiple
embeddeds). Each embedded renders into its own `embeddedRenderPrivate_`
texture and composites over the skipped slot. Only embeddeds whose anchor
line currently resolves to a viewport row in `[0, rows)` are drawn; the rest
are invisible until the user scrolls them into view.

**Alt-screen.** Hidden entirely while the parent is on alt-screen (no
persistent scrollback for the anchor to resolve against). New embeddeds are
refused in that mode; existing ones re-appear when the parent exits alt.

**Lifetime.** When the anchor line evicts past the archive cap,
`Document::onLineIdEvicted` fires from inside the Document mutex. Because the
render thread takes (renderMutex → terminalMutex) during snapshot capture,
the callback cannot take the render mutex itself (cyclic lock order).
Instead it moves the embedded's `unique_ptr` onto a per-Terminal pending
queue; the main-thread tick drains the queue (after the terminal mutex has
released), mirrors the removal into the render shadow copy under the render
mutex, stages a `DestroyEmbeddedState` op, and defers the Terminal to the
graveyard. Explicit `close()` from JS takes the same graveyard path directly.

**Focus.** The `FocusPopup` action cycles `pane → popups → embeddeds →
pane`. Click-to-focus via the same Input hit-test (skipped mid-drag so a
selection initiated on the parent crosses the embedded region). Esc on a
focused embedded defocuses back to the parent — intentionally different from
popups, which forward Esc to their content; applets use `q` / Ctrl-C for
their own cancel.

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
5. `dstSrcIdx` records the source row each dst row came from (first-src-wins;
   wrap-created rows inherit via a post-pass propagation of -1 sentinels).
6. Pre-existing `archive_` is cleared after being consumed as source, then
   refilled authoritatively from dst rows that exceed tier-1 capacity.
   (Before this clear, the archive duplicated content — stale old-width
   entries at the front plus re-wrapped dst rows appended, breaking both
   rendering and line-id monotonicity.)
7. Result installed as new ring. Line IDs rebuilt by mapping each dst row to
   `savedLineIds[dstSrcIdx[i]]`; blank ring tail gets fresh mints from the
   monotonic counter (guaranteed > all saved IDs). Vector stays non-decreasing.

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
- `D` — command finished (with exit code); stored on the per-command `CommandRecord`

**Per-command record**: an uncapped `std::deque<CommandRecord>` tracks exit
code, timing, captured command text (lazy via `getTextFromLines`), captured
output (lazy), and working directory per completed command. Each record stores
line IDs (`promptStartLineId`, `commandStartLineId`, `outputStartLineId`,
`outputEndLineId`) rather than positional row IDs, so references survive
reflow. Ring prunes in lockstep with archive eviction: a record is dropped
only when `Document::firstAbsOfLine(promptStartLineId)` returns -1 (line fully
evicted). Cells also keep the per-cell `semanticType` tag (Output / Input /
Prompt) in `CellAttrs` so content survives reflow and tier-2 archival — the
serializer re-emits `\e]133;A/B/C\e\\` transitions at segment boundaries and
the parser restores them on reload.

**Features using markers**:
- **Jump to prompt**: `scrollToPrompt(direction)` scans history + screen for
  `PromptStart` markers, scrolls the viewport, and sets `mSelectedCommandId`
  to the landed-on command. Navigation is relative to the current selection
  when one exists, not the viewport top. Keybindings: Cmd+Up/Down (macOS),
  Ctrl+Alt+Z/X (Linux).
- **Click-to-select** (Cmd+Click / Ctrl+Click): hits `commandForLineId` and
  sets `mSelectedCommandId`. The render path resolves the selection to abs
  rows via `firstAbsOfLine`/`lastAbsOfLine` each frame and the compute
  shader emits a 4-rect outline via new `TerminalComputeParams.selection_*`
  fields. Outline color is `command_outline_color` in config; live-reload
  repaints held textures. Escape (no modifiers) clears the selection when
  one exists. Alt-screen entry clears the selection. Split as
  `Action::SelectCommand` distinct from `Action::OpenHyperlink` — default
  bindings fire both on the same stroke.
- **Select command output**: `selectCommandOutput()` finds the `OutputStart`
  region around the viewport center and selects all text to the next
  `PromptStart`.
- **Prompt-aware reflow**: when OSC 133 markers are present, prompt lines can
  be blanked before reflow to prevent rprompt content from causing wrapping.
- **Scripting**: `pane.lastCommand`, `pane.commands`, `pane.selectedCommandId`,
  `pane.selectCommand(id)`, `commandComplete` event, and
  `commandSelectionChanged` event expose the command ring to JS. Command
  objects carry `rowId` handles on their position fields.

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
font = ""               # empty = system default via fontconfig / CoreText
font_size = 20.0
bold_strength = 0.04
scrollback_lines = -1   # -1 = infinite
divider_color = "#3d3d3d"
divider_width = 1

[tab_bar]
style = "auto"          # auto | visible | hidden
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

Platform-specific codepoint fallback, two-pass on both platforms (same strategy
as WezTerm):

**Linux (`src/FontFallback_FontConfig.cpp`)**:
- **Pass 1**: monospace fonts only (FC_MONO / FC_DUAL / FC_CHARCELL), using
  `FcFontList` (database order, no sort-scoring bias).
- **Pass 2**: any font if no monospace covers the codepoint.

**macOS (`src/FontFallback_CoreText.mm`)**:
- **Pass 1**: prefer monospace fonts (`kCTFontMonoSpaceTrait`) via
  `CTFontCopyDefaultCascadeListForLanguages` / `CTFontCreateForString`.
- **Pass 2**: any font returned by CoreText's cascade list.

Italic/bold variant lookup uses the same platform split
(`FontResolver_CoreText.mm` / `FontResolver_FontConfig.cpp`), resolving real
`kCTFontItalicTrait` / `FC_SLANT_ITALIC` files where available and otherwise
synthesizing via HarfBuzz (`hb_font_set_synthetic_slant` / `synthetic_bold`).

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
| `screenshot [--target <pane\|id>] [--cell x,y,w,h]` | Capture PNG of the window, a pane, or a cell rectangle |
| `key <key> [<mod>...]` | Inject a named key event with optional modifiers |
| `inject <text>` | Inject text into the active terminal |
| `feed <path> [repeat]` | Feed the contents of a file into the active terminal (optionally N times — benchmark fixture) |
| `wait-idle [timeout_ms [settle_ms]]` | Block until the parser is quiet and a frame has been drawn — pairs with `feed` for benchmark runs |
| `action <name> [args...]` | Dispatch a registered action (built-in or script-registered) |
| `stats` | GPU pool and pane memory usage + `obs` observability counters (JSON) |
| `logs` | Stream log messages (Ctrl+C to stop) |

Stats response includes texture pool (total/in-use/free/bytes), compute pool,
observability counters (`obs`), and per-tab/per-pane information (dimensions,
held texture size, divider buffer).

The IPC socket is only created when `mb` runs in `--test` mode, so production
builds don't expose the endpoint.

---

## 16. Terminal Capability Queries (implemented)

### Device Attributes

- **DA1** (`CSI c` / `CSI 0 c`): responds `ESC [ ? 64;1;2;6;22 c` (VT420, 132-col, printer, selective erase, ANSI color).
- **DA2** (`CSI > c`): responds `ESC [ > 64;2500;0 c` (VT500-class, xterm-compatible version 2500).
- **XTVERSION** (`CSI > q`): responds `DCS > | MasterBandit(0.1) ST`.

### XTGETTCAP

`DCS + q <hex-names> ST` — queries terminfo-style capabilities by name. Names are semicolon-separated and hex-encoded. Each capability gets a separate response:

- Found: `DCS 1 + r <hex-name> = <hex-value> ST`
- Not found: `DCS 0 + r <hex-name> ST`

**Supported capabilities:**

| Name | Meaning |
|---|---|
| `TN` / `name` | Terminal name (`MasterBandit`) |
| `Tc` | True-color support (tmux extension) |
| `Su` | Styled/colored underlines |
| `fullkbd` | Kitty full keyboard protocol |
| `XF` | Focus-in/out events (mode 1004) |
| `Sync` | Synchronized output / mode 2026 (tmux/kitty extension) |
| `Smulx` | Curly/styled underlines (`CSI 4:N m`) |
| `Setulc` | Underline color (`CSI 58:2:...m`) |
| `setrgbf` / `setrgbb` | RGB foreground/background (`CSI 38:2:...m` / `CSI 48:2:...m`) |
| `BE` / `BD` / `PS` / `PE` | Bracketed paste enable/disable and delimiters |
| `fe` / `fd` | Focus event enable/disable |
| `XR` / `RV` | XTVERSION / DA2 query sequences |
| `smcup` / `rmcup` | Alternate screen |
| `smkx` / `rmkx` | Application keypad mode |
| `Ss` / `Se` | DECSCUSR set/reset cursor style |
| `civis` / `cnorm` / `cvvis` | Cursor visibility |
| `cup` / `hpa` / `vpa` | Cursor positioning |
| `cuu` / `cud` / `cuf` / `cub` (+ `1` variants) | Cursor movement |
| `clear` / `ed` / `el` / `el1` / `ech` | Erase operations |
| `csr` / `ind` / `ri` / `indn` / `rin` | Scroll region and scrolling |
| `il` / `il1` / `dl` / `dl1` / `dch` / `dch1` / `ich` | Insert/delete lines and characters |
| `bold` / `dim` / `rev` / `blink` / `sitm` / `ritm` / `smul` / `rmul` / `smso` / `rmso` / `smxx` / `rmxx` / `sgr0` | SGR attributes |
| `setaf` / `setab` | 256-color foreground/background |
| `rep` | REP — repeat preceding character |
| `sc` / `rc` | Save/restore cursor |
| `u6` / `u7` / `u8` / `u9` | CPR and DA query sequences (xterm convention) |
| `colors` | 256 |
| `it` | Tab stop interval (8) |

**MasterBandit-specific capabilities:**

| Name | Meaning |
|---|---|
| `mb-query-popup` | Supports OSC 58237 popup pane API |
| `mb-query-applet` | Supports OSC 58237 JS applet loading |

Shell helper:
```sh
xtgettcap() { printf "\eP+q$(printf '%s' "$1" | xxd -p)\e\\"; }
xtgettcap Tc          # → 1+r5463  (found, boolean)
xtgettcap mb-query-popup  # → 1+r6d622d71756572792d706f707570
```

---

## 17. Async PTY Parsing & Concurrency Model (implemented)

### Threads

| Thread | Owner | Lifetime |
|---|---|---|
| **Main / event-loop** | `PlatformDawn::exec()` | App lifetime |
| **Render** | `RenderThread` | App lifetime |
| **Worker pool** | `RenderEngine::renderWorkers_` (shared) | App lifetime |

The worker pool is owned by `RenderEngine` (`RenderEngine::workers()` accessor)
and used by both the render thread (parallel row resolution / shaping) and
the main thread (async PTY parsing). It supports two submission modes:

- `submit(fn)` — fire-and-forget async task; returns immediately.
- `dispatch(items, fn)` — synchronous parallel batch; blocks the caller until
  every item in *this batch* finishes. Concurrent dispatches and submits run
  in parallel; one batch's wait does not block another.

### PTY parsing pipeline

PTY data flows from the kernel to the terminal grid in three stages, each
running on a different thread:

1. **fd-readable callback** (main thread). `Terminal::readFromFD()` drains
   the kernel buffer in 64 KB chunks and appends to `mReadCoalesceBuffer`
   under `mReadBufferMutex` (a small leaf-level lock). No parsing.
2. **Per-tick dispatch** (main thread). On every event-loop iteration,
   `onTick` calls `Terminal::queueParse(submit)` for each PTY. queueParse
   does an atomic single-flight check (`mParseInFlight`) and submits a
   parse closure to the worker pool. If a worker is already parsing this
   Terminal, queueParse returns immediately — the in-flight worker will
   loop and pick up newly-arrived bytes itself.
3. **Parse worker** (worker thread). The closure swaps the coalesce
   buffer out under `mReadBufferMutex`, calls `injectData` on the swapped
   bytes (which acquires `mMutex` for the duration of the parse), and
   loops to drain any bytes that arrived during the parse. Releases
   `mParseInFlight` under `mReadBufferMutex` to avoid the
   "parser-just-released-but-bytes-arrived-first" stranding race.

This decouples the main thread from heavy parse work. Pre-async, a flooding
producer would block the main event loop for the duration of an `injectData`
batch (potentially seconds), starving input. Post-async, the main thread's
per-tick cost is bounded to atomic flag checks and short worker-submission
calls.

### TerminalCallbacks safety on the worker

Most `TerminalCallbacks` already used `eventLoop_->post(...)` to defer to
main, so they're trivially worker-thread-safe. Three sync callbacks needed
adapting:

- `pasteFromClipboard` (OSC 52 c=? query) — bounces through
  `PlatformDawn::runOnMain()`, which posts to the event loop and waits on a
  `std::promise`. Adds latency only to the rare OSC52 paste-query response.
- `isDarkMode` (mode 2031) — same `runOnMain` bounce.
- `customTcapLookup` (DCS+q) — same bounce; script engine is main-only.

Output and input filters (`shouldFilterOutput`, `filterOutput`,
`shouldFilterInput`, `filterInput`) are different: the *check* must be
fast (called once per parse batch / writeToOutput), but the *invocation*
runs JS. Solved with per-pane `std::shared_ptr<std::atomic<bool>>` flags
maintained by `Engine::outputFilterFlag(PaneId)` /
`Engine::inputFilterFlag(PaneId)`. The filter closure captures the
shared_ptr (lifetime-safe past pane teardown), atomic-loads it for
`shouldFilter*`, and bounces through `runOnMain` only when the actual
filter runs.

### Locking model

The terminal subsystem maintains exactly **two** mutexes per Terminal plus
a small set of atomics:

- **`TerminalEmulator::mMutex`** (recursive). Protects all parse-mutated
  state: grid, document, cursor, `mState` fields, command ring, selection,
  hyperlink registry, title/icon stacks, embeddeds map. Held by the parse
  worker for the duration of `injectData`; by the render thread during
  snapshot capture; by main-thread one-off readers (mouse handlers, JS
  getters, action dispatch, OSC reply construction).
- **`Terminal::mReadBufferMutex`**. Leaf-level. Covers
  `mReadCoalesceBuffer` and the `mParseInFlight` release. Never held while
  taking another lock.

Hot main-thread paths (`buildRenderFrameState`, `onBlinkTick`, eviction
polling) **never** take `mMutex`. They consume lock-free atomic snapshots:

| Snapshot | Type | Updated when |
|---|---|---|
| `usingAltScreen()` | `atomic<bool>` (mirror of `mUsingAltScreen`) | mode 1049/47 toggle, RIS |
| `currentTitle()` / `currentIcon()` | `atomic<shared_ptr<const string>>` | OSC 0/1/2 set, XTWINOPS push/pop |
| `embeddedSnapshot()` | `atomic<shared_ptr<const vector<EmbeddedView>>>` | createEmbedded / extractEmbedded / eviction / onFullReset / parent resize cascade |
| `focusedEmbeddedLineId()` | `atomic<uint64_t>` | focus mutator + eviction |
| `hasEvictedEmbeddeds()` | `atomic<bool>` | eviction adds, drain clears |
| `parseInFlight()` | `atomic<int>` | queueParse + worker exit |

Mutators that change underlying state call `publishTitleAtomic()` /
`publishIconAtomic()` / `publishEmbeddedSnapshot()` (or store directly to
the simpler atomics) **under `mMutex`**, so consumers see consistent
snapshots without locking.

### Lifetime of snapshot pointers

`EmbeddedView::term` is a raw `Terminal*`. Lifetime is guaranteed by the
existing **graveyard** (`src/platform/Graveyard.h`):

- An extracted Terminal is staged into the graveyard with a frame stamp
  (taken under `renderThread_->mutex()`) and a `parseInFlight() == 0`
  predicate.
- `Graveyard::sweep()` only frees an entry when *both* `completedFrames >
  stamp` AND the predicate returns true.
- Render frames advance the stamp counter; main-thread consumers that
  copy raw pointers into `RenderPaneEmbeddedInfo` are bounded by the same
  frame.
- Parse workers are bounded by `parseInFlight`.

### Shutdown ordering

`~PlatformDawn` follows a precise sequence to avoid use-after-free between
the worker pool, the event loop, and graveyard'd Terminals:

1. `platformShutdown()` joins platform-wide background workers (DBus etc.).
2. `renderThread_->stop()` joins the render thread.
3. **Spin-wait** with `eventLoop_->drainPosts()` on each iteration until
   every Terminal's `parseInFlight()` reads 0. The drainPosts is required
   because a parse worker may be parked inside `runOnMain()` waiting for
   the main thread to service a post (pasteFromClipboard / customTcap);
   without draining, the worker hangs and the count never reaches zero.
4. Final `drainPosts` to catch any post that completed during the last
   iteration's check.
5. `graveyard_.drainAll()` is now safe — no concurrent reader.
6. `renderEngine_->shutdown()` releases GPU resources.
7. Window destruction.
8. RenderEngine reset (joins the worker pool — guaranteed empty by step 3).

### Per-tick consumer pattern

`PlatformDawn::buildRenderFrameState()` runs on every tick and demonstrates
the full pattern:

```cpp
rpi.focusedEmbeddedLineId = pane->focusedEmbeddedLineId();  // atomic load
rpi.onAltScreen           = pane->usingAltScreen();         // atomic load
pane->forEachEmbedded([&rpi, focusedLine = rpi.focusedEmbeddedLineId]
                      (uint64_t lineId, Terminal& em) {     // atomic-snapshot iter
    rpi.embeddeds.push_back({lineId, em.height(), &em,
                             lineId == focusedLine});
});
```

No `mMutex` acquired. Per-tick cost is bounded regardless of how busy any
parse worker is.

---

## 18. Project Structure

```
src/
  main.cpp                         — entry point, config loading, CLI dispatch
  Action.h/cpp                     — Action variant (all built-in + script actions)
  Bindings.h/cpp                   — keybinding parsing and sequence matching
  ClickDetector.h/cpp              — multi-click detection for mouse bindings
  CLIClient.h/cpp                  — `mb --ctl` client (IPC over UDS)
  DebugIPC.h/cpp                   — IPC server surface (screenshot/key/stats/feed/wait-idle/…)
  Config.h/cpp                     — TOML config loading (glaze)
  ColrAtlas.h/cpp                  — COLRv1 bucketed tile atlas (32/64/128/256/512 px)
  ColrEncoder.h/cpp, ColrTypes.h   — COLRv1 paint-graph encoding for GPU rasterization
  FontResolver.h                   — family → file resolution interface
  FontResolver_CoreText.mm         — macOS implementation (CoreText)
  FontResolver_FontConfig.cpp      — Linux implementation (fontconfig)
  FontFallback.h                   — codepoint fallback interface
  FontFallback_CoreText.mm         — macOS implementation (two-pass CoreText)
  FontFallback_FontConfig.cpp      — Linux implementation (two-pass fontconfig)
  Observability.h                  — lightweight counter/histogram registry
  ProceduralGlyphTable.h           — box drawing / powerline / shade procedural glyphs
  Layout.h/cpp                     — binary split tree for pane layout
  Pane.h/cpp                       — terminal container with popup support
  Tab.h/cpp                        — tab container with overlay support
  text.h/cpp                       — HarfBuzz shaping, font registry, glyph atlas, SheenBidi BiDi
  WorkerPool.h                     — small fixed-size thread pool for row resolve/shape
  renderer_utils.h, Utils.h        — shared render helpers
  platform/                        — GPU rendering, windowing (OBJECT library)
    PlatformDawn.h/cpp             — platform singleton, init, render thread, deferred mutation
    Platform_EventLoop.cpp         — event-loop tick, PTY polling, timers, render wakeup
    Platform_Input.cpp             — keyboard/mouse event routing
    Platform_Actions.cpp           — action dispatch
    Platform_Render.cpp            — renderFrame, resolveRow, snapshot capture, shaping phase
    Platform_Tabs.cpp              — tab/pane lifecycle, dividers, buildTerminalCallbacks
    Platform_TabBar.cpp            — tab bar init and rendering
    Platform_Debug.cpp             — gridToJson, statsJson, debug IPC handlers
    PlatformUtils.h, PlatformUtils_macOS.mm, PlatformUtils_Linux.cpp — OS helpers
    Renderer.h/cpp                 — GPU pipelines, render passes, compositing
    ComputeStatePool.h/cpp         — GPU buffer pool for compute pass
    ComputeTypes.h                 — ResolvedCell, GlyphEntry, TerminalComputeParams
    TexturePool.h/cpp              — GPU texture pool with LRU eviction
    InputTypes.h                   — Key enum, KeyEvent, MouseEvent, modifiers
  terminal/                        — terminal emulation (OBJECT library)
    TerminalEmulator.h/cpp         — VT parser core: state machine, CSI, onAction, mMutex
    Terminal.h/cpp                 — PTY management (fork, read, write, resize)
    TerminalSnapshot.h/cpp         — viewport snapshot captured by the render thread
    TerminalOptions.h              — terminal creation options
    KittyKeyboard.cpp              — kitty keyboard protocol + key encoding
    KittyGraphics.cpp              — kitty image protocol, animation, placements
    MouseAndSelection.cpp          — mouse event handling, text selection
    SGR.cpp                        — Select Graphic Rendition attribute parsing
    OSC.cpp                        — OSC processing, iTerm2 images (1337), clipboard, notifications
    DCS.cpp                        — DCS processing (XTGETTCAP, DECRQSS)
    Document.h/cpp                 — scrollback: ring buffer + compressed archive
    CellGrid.h/cpp                 — simple cols×rows cell array
    CellTypes.h                    — Cell, CellAttrs, CellExtra structs
    IGrid.h                        — abstract grid interface
    Utf8.h, Wcwidth.h              — UTF-8 decoding + East Asian Width tables
  eventloop/                       — platform-specific event loops and window backends
    EventLoop.h                    — abstract fd watch / timer interface
    mac/Window_cocoa.mm, …         — macOS Cocoa run loop + NSWindow
    xcb/Window_xcb.cpp, …          — Linux X11 / XCB
    epoll/, kqueue/                — fd/timer primitives
  script/                          — QuickJS-ng scripting
    ScriptEngine.h/cpp             — JS runtime, mb.* API surface, lifecycle
    ScriptPermissions.h/cpp        — permission bitmask, allowlist, hash verification
    ScriptFsModule.cpp             — `mb:fs` file API (permission-gated)
    ScriptWsModule.cpp             — WebSocket client module
    …                              — other JS modules
  shaders/                         — WGSL sources (compute + fragment)
assets/
  scripts/
    applet-loader.js               — built-in: OSC 58237 applet loading + permission prompts
    command-palette.js             — built-in: fuzzy command palette (Cmd+Shift+P)
    modules/                       — built-in JS modules (e.g. `mb:tui`)
examples/
  popup-demo.js                    — example popup script
tests/
  TestTerminal.h                   — lightweight TerminalEmulator wrapper for tests
  MBConnection.h/cpp               — helper for tests that spawn `mb` and drive it over IPC
  reference/                       — reference PNGs for rendering tests
  test_*.cpp                       — doctest test files
```

---

## 19. Testing (implemented)

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

## 20. Scripting Engine (implemented)

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
