# MasterBandit Terminal — Design

## 1. Text Rendering (implemented)

GPU-accelerated glyph rendering via hb-gpu / Slug. Glyph outlines are encoded
CPU-side into a storage buffer atlas; the GPU fragment shader rasterizes directly
from Bézier curve data — no bitmap textures.

**Atlas**: `array<vec4<i32>>` storage buffer, 1M texels (4 MB) pre-allocated per
font. Each texel holds 4 int32 values widened from the hb-gpu int16-pair encoding.

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
   pass.
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
              │           └── popups_: Vec<PopupPane>  (OSC 999)
              └── overlays_: Vec<Terminal>  ← full-screen overlays (no tab bar entry)
```

### TerminalEmulator / Terminal split

- **`TerminalEmulator`**: VT parser + cell state + selection + image registry.
  No PTY. Can be used standalone for popup panes. Callbacks via `TerminalCallbacks`.
- **`Terminal`** extends `TerminalEmulator`: adds PTY management (fork, masterFD,
  `readFromFD`, resize+ioctl). PTY exit calls `Platform::terminalExited()` which
  closes the pane (or tab if last pane, or quits if last tab).

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
- Both panes marked dirty so cursor type (solid → hollow or vice-versa) re-renders.

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

### Popup panes (OSC 999)

Shell-driven floating cell grids. No PTY — content written via escape sequence.
Protocol: `OSC 999 ; create/write/focus/blur/destroy ; id=<id> ; ... ST`.
Keyboard routing: focused popup receives keys; responses go back via owning
terminal's PTY.

### Resource pools

- **`TexturePool`**: GPU textures (RenderAttachment|CopySrc). Best-fit acquire,
  immediate release, byte-budget LRU eviction.
- **`ComputeStatePool`**: sets of 5 compute buffers + bind group. Same
  acquire/release/eviction pattern. Lives inside `Renderer`.

---

## 8. Keybindings / Actions (implemented)

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

## 9. Configuration (implemented)

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

## 10. Font Fallback (implemented)

Two-pass fontconfig strategy (same as WezTerm):
- **Pass 1**: monospace fonts only (FC_MONO / FC_DUAL / FC_CHARCELL), using
  `FcFontList` (database order, no sort-scoring bias).
- **Pass 2**: any font if no monospace covers the codepoint.

Unicode whitespace codepoints are excluded from fallback (no visible glyph needed).

Bundled fallback: **Symbols Nerd Font Mono** (OFL licensed) copied alongside the
binary, loaded as a registered fallback for powerline glyphs (U+E0B0–E0B3) and
other symbols.

---

## 11. Debug IPC (implemented)

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

## 12. Testing (implemented)

Unit test suite using doctest (`tests/`). `TestTerminal` wraps `TerminalEmulator`
with no PTY or GPU — feeds escape sequences via `injectData`, asserts on cell grid
state. 4096-line scrollback enabled by default.

Coverage: text output, cursor movement (CUP/CUU/CUD/CUF/CUB/CHA/CNL/CPL/VPA),
SGR (all attributes, 16/256/truecolor fg/bg, inverse, turn-off codes), screen
operations (ED/EL/SU/SD/DECSTBM/DCH/ICH/IL/DL/ECH), terminal modes (alt screen,
mouse 1000/1002/1003, bracketed paste, sync output, RIS, DA/XTVERSION), scrollback
viewport, OSC 0/1/2 title/icon, wide characters (CJK, emoji), SGR inverse
color swap semantics.

Run with: `./build/bin/mb-tests`
