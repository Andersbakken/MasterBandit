#pragma once

#include <CellTypes.h>
#include <TerminalEmulator.h>
#include <cstdint>
#include <utility>
#include <vector>

// Render-thread view of Terminal state. Populated under the Terminal mutex
// in `update()`; read without a lock on the render thread. See
// RENDER_THREADING.md §Snapshot Type.
//
// Phase 1 coverage: viewport cells + extras, cursor, default colors, viewport
// offset + history size (for abs-row conversion), selection, sync-output flag.
// Image registry is deliberately deferred to Phase 2 — the renderer still
// reads the live registry under the Terminal mutex in Phase 1.
struct TerminalSnapshot {
    // Dimensions of the viewport.
    int rows { 0 };
    int cols { 0 };

    // Scrollback position and size, both in rows. `absRow = viewportRow + historySize - viewportOffset`.
    int viewportOffset { 0 };
    int historySize { 0 };

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

    // Mode 2026 — when true the render thread should re-present the prior
    // frame instead of updating.
    bool syncOutputActive { false };

    // Monotonic counter — bumped by each successful update(). Useful for
    // debugging and for ensuring a render sees a new snapshot.
    uint64_t version { 0 };

    // Copies state from `term` into this snapshot. Caller must hold
    // `term.mutex()` for the duration of the call. Returns false if sync
    // output is active and the snapshot was not updated (caller should
    // re-present prior frame). Clears per-row dirty flags on `term.grid()`
    // for rows that were re-copied.
    bool update(TerminalEmulator& term);

private:
    // Cached geometry from the last successful update — used to detect
    // structural changes (resize, scroll jump) that force a full re-copy.
    int lastRows_ { 0 };
    int lastCols_ { 0 };
    int lastViewportOffset_ { 0 };
    int lastHistorySize_ { 0 };
};
