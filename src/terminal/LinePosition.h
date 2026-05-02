#pragma once

#include <cstdint>

// Width-independent cell coordinate inside a logical line.
//
// Selection, search, marks, and any other coordinate that needs to
// survive a width-change reflow stores positions as `LinePosition`
// instead of `(absRow, col)`. The `lineId` is stable across reflow;
// `cellOffset` is the count of cells (DWC = 2 cells) from the start
// of the logical line, also reflow-stable.
//
// Projection to the on-screen `(row, col)` happens at render or query
// time via the wrap calculator at the current display width.
struct LinePosition {
    uint64_t lineId = 0;
    int      cellOffset = 0;

    constexpr bool valid() const { return lineId != 0 && cellOffset >= 0; }
};

constexpr bool operator==(LinePosition a, LinePosition b) {
    return a.lineId == b.lineId && a.cellOffset == b.cellOffset;
}
constexpr bool operator!=(LinePosition a, LinePosition b) { return !(a == b); }
