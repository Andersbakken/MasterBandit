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
cursor/selection/underlines. Rects render before text so glyphs overlay.

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

## 4. Ligatures (to implement)

Optional, user-configurable.

**Principle**: the cell grid stores raw codepoints and attributes only — no
ligature state is persisted. Ligature detection happens at render time.

**Shaping pass** (CPU, per dirty row):

1. Accumulate runs of adjacent cells sharing identical attributes.
2. Feed each run to HarfBuzz.
3. Inspect cluster output to detect ligature substitutions.
4. Ligature glyph renders in the first cell, spanning N cell widths; the
   remaining N−1 cells become spacers.

**Attribute boundaries break ligatures**: if adjacent cells differ in fg color or
other attributes, they belong to separate shaping runs.

**Caching**: shaping result cached per row; invalidated when the row's dirty bit
is set.

---

## 5. GPU Pipeline (implemented)

Per-frame flow:

1. **CPU**: shape dirty rows → produce a resolved cell buffer. Each entry contains
   glyph ID, atlas offset, cell span, and packed color.
2. **Upload**: resolved cells written to a `ComputeState` GPU storage buffer
   (acquired from `ComputeStatePool`).
3. **Compute shader**: reads resolved cells + font metrics, emits `SlugVertex`
   entries into a vertex buffer. Also emits `RectVertex` for non-default backgrounds.
4. **Render passes**: rect pass (backgrounds, cursor, selection) then Slug text
   pass.
5. **Texture pool**: rendered output goes into a `PooledTexture` from
   `TexturePool`. Textures are held between frames when content is unchanged and
   re-acquired when dirty. Both pools use byte-budget LRU eviction.
6. **Compositor**: composites all visible pane textures plus the tab bar texture
   onto the swapchain via `CopyTextureToTexture`. An explicit viewport is set on
   each render pass to ensure correct NDC mapping when a pool texture is larger
   than the content area.

---

## 6. Dirty Tracking (implemented)

- Per-row dirty bit in the grid.
- Per-pane `dirty` flag in `PaneRenderState` — set on terminal update events.
- Only dirty panes acquire a new pool texture and re-render.
- Clean panes reuse their held texture and are composited without re-rendering.
- Tab bar has its own `tabBarDirty_` flag; re-renders on tab add/remove/switch,
  title/icon/progress change, and animation frame advance.

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
              │           └── popups_: Vec<PopupPane>  (OSC 999)
              └── overlays_: Vec<Terminal>  ← full-screen overlays (no tab bar entry)
```

### TerminalEmulator / Terminal split

- **`TerminalEmulator`**: VT parser + cell state + selection + image registry.
  No PTY. Can be used standalone for popup panes. Callbacks via `TerminalCallbacks`.
- **`Terminal`** extends `TerminalEmulator`: adds PTY management (fork, masterFD,
  `readFromFD`, resize+ioctl).

### Tab bar

- Rendered as a 1-row cell grid using the same compute/text/rect GPU pipeline.
- Powerline style: U+E0B0 chevron separators via bundled Symbols Nerd Font Mono.
- Per-tab: index `[N]`, optional icon (OSC 1), title (OSC 2), progress glyph
  (OSC 9;4) with indeterminate animation.
- Configurable: style (powerline/hidden), position (top/bottom), font, font size,
  colors. Stored in `[tab_bar]` config section.
- Left-click: switch tab. Middle-click: close tab (no-op if last tab).

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

## 8. Configuration (implemented)

TOML config at `$XDG_CONFIG_HOME/MasterBandit/config.toml`. Parsed via glaze.

```toml
font = "Inconsolata"
font_size = 20.0
bold_strength = 0.04
scrollback_lines = -1   # -1 = infinite

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
```

Font resolution uses fontconfig (Linux) or CoreText (macOS) for family→file
lookup, with monospace-first fallback for missing glyphs.

Bold: uses a real bold font variant if found; otherwise synthesizes via
`hb_font_set_synthetic_bold`.

---

## 9. Font Fallback (implemented)

Two-pass fontconfig strategy (same as WezTerm):
- **Pass 1**: monospace fonts only (FC_MONO / FC_DUAL / FC_CHARCELL), using
  `FcFontList` (database order, no sort-scoring bias).
- **Pass 2**: any font if no monospace covers the codepoint.

Unicode whitespace codepoints are excluded from fallback (no visible glyph needed).

Bundled fallback: **Symbols Nerd Font Mono** (OFL licensed) copied alongside the
binary, loaded as a registered fallback for powerline glyphs (U+E0B0–E0B3) and
other symbols.
