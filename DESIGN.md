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

Current terminal data model uses string-based `Line { data, lineBreaks, commands }`
with sparse `Command` annotations. This will be replaced by the cell grid below.

---

## 2. Cell Grid (to implement)

Replace the current `Line { string, commands }` model with a `cols × rows` cell
array providing O(1) random access for cursor-addressed writes.

**Target cell**: ~12 bytes (foot-inspired)

| Field | Size | Contents |
|---|---|---|
| `char32_t wc` | 4 B | Unicode codepoint |
| packed attrs | 8 B | 24-bit fg, 24-bit bg, 2-bit color mode per color, 1-bit flags: bold, italic, underline, strikethrough, blink, inverse, dim |

Rare attributes (hyperlinks, combining characters, image refs) are stored in a
lazy-allocated side table keyed by cell coordinate — keeps the hot path small.

**Wide characters**: first cell holds the glyph, subsequent cells are marked as
spacers (same model reused for ligatures).

---

## 3. Scrollback (to implement)

Two-tier design (kitty-inspired).

### Tier 1 — Ring buffer of full cell rows

- Visible screen + bounded history (configurable row count, power-of-2 for fast
  modulo).
- Each row is a contiguous array of cells, so ~2.4 KB for a 200-column line.
- Mutable; direct target of terminal writes.

### Tier 2 — Compressed archive

- Lines re-encoded as UTF-8 text + inline ANSI SGR escape sequences.
- Much more compact: ~100 bytes/line typical vs ~2.4 KB for full cells.
- Read-only. Re-parsed on demand when the user scrolls back far enough.
- Supports deep/unlimited scrollback without proportional memory cost.

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
   remaining N−1 cells become spacers (same wide-char model).

**Attribute boundaries break ligatures** (foot-style): if adjacent cells differ in
fg color or other attributes, they belong to separate shaping runs even if the
font contains a ligature for that sequence. This respects the user's explicit
color/style choices.

**Caching**: shaping result cached per row; invalidated when the row's dirty bit
is set.

---

## 5. GPU Pipeline (to implement)

Per-frame flow:

1. **CPU**: shape dirty rows → produce a resolved row buffer. Each entry contains
   glyph ID, atlas offset, cell span, and packed color.
2. **Upload**: resolved rows written to a GPU storage buffer.
3. **Compute shader**: reads resolved rows + font metrics, emits `SlugVertex`
   entries into a vertex buffer. Also emits `RectVertex` for cells with
   non-default background colors.
4. **Render passes**: rect pass (backgrounds, cursor, selection) then Slug text
   pass (existing fragment shader, unchanged).

**Common case** (no dirty rows): skip CPU shaping and compute dispatch entirely;
reuse the previous frame's vertex buffer.

---

## 6. Dirty Tracking (to implement)

- Per-row dirty bit.
- Only dirty rows are re-shaped (HarfBuzz) and have vertices regenerated.
- Clean rows reuse cached shaping results and existing vertex data.
- Grid-level "all dirty" flag for full-screen redraws: resize, alternate screen
  switch, bulk scroll.
