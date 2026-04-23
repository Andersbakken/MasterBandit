#pragma once

#include <CellTypes.h>
#include <TerminalEmulator.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Render-thread view of Terminal state. Populated under the Terminal mutex
// in `update()`; read without a lock on the render thread.
//
// Covers: viewport cells + extras, cursor, default colors, viewport offset +
// history size (for abs-row conversion), selection, sync-output flag, image
// views (kitty graphics placements + current-frame RGBA pointer held alive
// via shared_ptr), and a visual-layout segment list describing the top-down
// ordering of rows and inline embeddeds inside the viewport. The renderer
// reads only this snapshot — it never dereferences the live Terminal during
// rendering.
struct TerminalSnapshot {
    // Dimensions of the viewport.
    int rows { 0 };
    int cols { 0 };

    // Scrollback position and size, both in rows. Legacy mirror of
    // term.viewportOffset() (derived from topLineId at snapshot time) and
    // term.document().historySize(). The segment list below is the
    // authoritative source for per-viewport-row absRow / pixel placement —
    // use segments[i].absRow rather than `historySize - viewportOffset + i`
    // whenever possible, since the segment walk accounts for embedded
    // displacement and topPixelSubY.
    int viewportOffset { 0 };
    int historySize { 0 };

    // Logical-line id at the top of the viewport. Derived from
    // `historySize - viewportOffset` at update() time. Remains stable across
    // embedded resizes — only a scroll / write advances it.
    uint64_t topLineId { 0 };

    // Sub-cell vertical offset for smooth scrolling, in pixels. 0 means
    // top-aligned on the first visible row. Positive values shift visible
    // content up (more of the next row revealed at the bottom). Phase 1
    // leaves this at 0 — no user gesture is wired yet.
    int topPixelSubY { 0 };

    // Visual-layout segment list. One entry per visible band (row or
    // embedded). `cellYStart` is in whole-cell units measured from the top
    // of the viewport; renderer multiplies by cellH to get pixels, then
    // subtracts `topPixelSubY` for smooth scroll. Populated in update().
    struct Segment {
        enum class Kind : uint8_t { Row, Embedded };
        Kind     kind       { Kind::Row };
        // Absolute row this segment corresponds to. For Row: the row itself.
        // For Embedded: the anchor row of the embedded terminal (covered by
        // the embedded visually, so not otherwise drawn).
        int      absRow     { -1 };
        // Logical-line id of `absRow`. For Row: the row's line id. For
        // Embedded: the anchor line id the embedded Terminal is keyed on.
        uint64_t lineId     { 0 };
        // How many cell rows this segment occupies vertically. Row = 1,
        // Embedded = embedded terminal's row count.
        int      rowCount   { 1 };
        // Top edge in whole-cell units, cumulative from the first segment.
        int      cellYStart { 0 };
    };
    std::vector<Segment> segments;

    // Find the segment containing viewport pixel y (measured from the pane's
    // top content edge, i.e. after padTop has been subtracted). cellH is the
    // pixel height of one cell row. Returns nullptr when y falls outside any
    // segment (e.g. past the last row or into the bottom padding).
    const Segment* segmentAtPixelY(int y, float cellH) const;

    // Cursor.
    int cursorX { 0 };
    int cursorY { 0 };
    TerminalEmulator::CursorShape cursorShape { TerminalEmulator::CursorBlock };
    bool cursorVisible { false };
    bool cursorBlinking { false };

    // Default (non-SGR) palette colors — for cursor, uncolored cells, clear rect.
    TerminalEmulator::DefaultColors defaults {};

    // Viewport cells, row-major, size = rows * cols. POD memcpy from the grid.
    std::vector<Cell> cells;

    // Per-row flag set when the row was re-copied this frame. Drives the
    // renderer's per-row shape cache invalidation.
    std::vector<uint8_t> rowDirty;

    // Per-row sorted extras. Only populated for rows that have any extras in
    // the underlying grid; otherwise the inner vector is empty.
    struct RowExtras {
        std::vector<std::pair<int, CellExtra>> entries;  // (col, extra), sorted by col
    };
    std::vector<RowExtras> rowExtras;

    // Selection. Mirrors TerminalEmulator::Selection so isCellSelected() can
    // be evaluated without calling back into Terminal.
    TerminalEmulator::Selection selection {};
    bool isCellSelected(int col, int absRow) const;

    // OSC 133 command highlight. Populated when a CommandRecord is selected
    // via Cmd+Click or keyboard nav. Resolved to absolute rows at snapshot
    // time; renderer converts to viewport-relative using historySize and
    // viewportOffset (same math as isCellSelected). Cleared on alt screen.
    struct SelectedCommandRegion {
        int startAbsRow;
        int startCol;
        int endAbsRow;
        int endCol;
    };
    std::optional<SelectedCommandRegion> selectedCommand;

    // Per-image view for rendering and animation scheduling. Populated from
    // TerminalEmulator::imageRegistry() during update() with the subset of
    // images needed this frame — visible (referenced by CellExtra in viewport
    // rows) plus any images with active animations (so animation scheduling
    // sees off-screen running images too).
    //
    // `entry` is a shared_ptr into the parser's registry: keeps the ImageEntry
    // alive across render without holding the Terminal mutex even if the
    // parser deletes its map entry. Scalar fields are copied by value at
    // snapshot time; placements map is copied so render iteration can't race
    // with parser insertion. `currentFrameRGBA` is a raw pointer into the
    // entry's (still-alive) rgba vector — safe because rgba content is
    // immutable after load, and vector<Frame> moves preserve inner buffer
    // identity.
    struct ImageView {
        std::shared_ptr<const TerminalEmulator::ImageEntry> entry;
        uint32_t pixelWidth { 0 }, pixelHeight { 0 };
        uint32_t cellWidth { 0 }, cellHeight { 0 };
        uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
        uint32_t currentFrameIndex { 0 };
        uint32_t totalFrames { 1 };  // 1 + extraFrames.size()
        uint32_t frameGeneration { 0 };
        uint32_t currentFrameGap { 40 };
        uint64_t frameShownAt { 0 };
        bool hasAnimation { false };
        const uint8_t* currentFrameRGBA { nullptr };
        size_t currentFrameRGBASize { 0 };
        std::unordered_map<uint32_t, TerminalEmulator::ImageEntry::Placement> placements;
    };
    std::unordered_map<uint32_t, ImageView> images;

    // Mode 2026 — when true the render thread should re-present the prior
    // frame instead of updating.
    bool syncOutputActive { false };

    // Monotonic counter — bumped by each successful update(). Useful for
    // debugging and for ensuring a render sees a new snapshot.
    uint64_t version { 0 };

    // Copies state from `term` into this snapshot. Acquires `term.mutex()`
    // internally for the duration of the copy. Returns false if sync output
    // is active and the snapshot was not updated (caller should re-present
    // prior frame). Clears per-row dirty flags on `term.grid()` for rows
    // that were re-copied.
    bool update(TerminalEmulator& term);

private:
    // Cached geometry from the last successful update — used to detect
    // structural changes (resize, scroll jump) that force a full re-copy.
    int lastRows_ { 0 };
    int lastCols_ { 0 };
    int lastViewportOffset_ { 0 };
    int lastHistorySize_ { 0 };
};
