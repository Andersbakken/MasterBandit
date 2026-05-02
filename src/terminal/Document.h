#pragma once

#include "IGrid.h"
#include "LineBuffer.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// Document holds the main-screen contents:
//   - Visible grid: small ring of screenHeight_ rows × cols_ cells (the
//     active screen). IL/DL/ICH/DCH/print/cursor mutate this directly.
//   - Scrollback: a LineBuffer of variable-length logical lines. Resize
//     is a no-op on storage; only the wrap cache invalidates.
//
// Scroll-out path: when scrollUp(0, screenHeight_, n) advances the ring,
// each evicted top row is appended to the scrollback with its EOL flag
// (Hard / Soft / Dwc). Soft-wrapped rows extend a partial scrollback line
// in place; Hard rows finalize and start a new logical line.
//
// Coordinates exposed through the IGrid interface are screen-row-based
// (visible grid only). The Document also exposes a coordinate space
// indexed by `abs row` = scrollback wrapped rows + visible grid rows;
// this is used by selection, line-id lookups, and JS callbacks.

class Document : public IGrid {
public:
    Document();
    Document(int cols, int screenHeight,
             int maxLogicalLines = LineBuffer::kDefaultMaxLogicalLines,
             int maxTotalCells   = LineBuffer::kDefaultMaxTotalCells);
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // --- IGrid interface (screen-relative coordinates, visible grid only) ---
    int cols() const override { return cols_; }
    int rows() const override { return screenHeight_; }

    Cell& cell(int col, int screenRow) override;
    const Cell& cell(int col, int screenRow) const override;
    Cell* row(int screenRow) override;
    const Cell* row(int screenRow) const override;

    void markRowDirty(int screenRow) override;
    void markAllDirty() override;
    void clearDirty(int screenRow) override;
    void clearAllDirty() override;
    bool isRowDirty(int screenRow) const override;
    bool anyDirty() const override;

    void clearRow(int screenRow) override;
    void clearRow(int screenRow, int startCol, int endCol) override;
    void scrollUp(int top, int bottom, int n) override;
    void scrollDown(int top, int bottom, int n) override;
    void deleteChars(int screenRow, int col, int count) override;
    void insertChars(int screenRow, int col, int count) override;

    const CellExtra* getExtra(int col, int screenRow) const override;
    CellExtra& ensureExtra(int col, int screenRow) override;
    void clearExtra(int col, int screenRow) override;
    void clearRowExtras(int screenRow) override;
    void markRowHasWide(int screenRow) override;

    // --- Scrollback API ---
    //
    // historySize() returns the count of WRAPPED rows in scrollback at the
    // current display width. historyRow(idx) returns one such wrapped row,
    // padded to cols_ cells. The pointer is into an internal buffer that's
    // overwritten on each call; copy if you need it.
    int historySize() const;
    const Cell* historyRow(int idx) const;
    const std::unordered_map<int, CellExtra>* historyExtras(int idx) const;
    bool isHistoryRowContinued(int idx) const;
    void clearHistory();

    // Total logical lines in scrollback (one per hard-broken or partial line).
    int scrollbackLogicalLines() const;

    // --- Viewport support ---
    // Returned pointers alias internal storage: valid until the next
    // mutation or the next viewportRow/historyRow call (which overwrites
    // the wrap buffer).
    const Cell* viewportRow(int viewRow, int viewportOffset) const;
    const std::unordered_map<int, CellExtra>* viewportExtras(int viewRow, int viewportOffset) const;

    // --- Resize ---
    struct CursorTrack {
        int srcX, srcY;   // input: cursor position as abs row index (history + screen)
        int dstX, dstY;   // output: cursor position in destination abs row index
    };
    void resize(int newCols, int newRows, CursorTrack* cursor = nullptr);

    // --- Per-row continuation flag (visible grid only) ---
    bool isRowContinued(int screenRow) const;
    void setRowContinued(int screenRow, bool v);

    // --- Logical-line IDs ---
    //
    // Each logical line gets a monotonic 64-bit ID at write time. Visible-
    // grid rows that share a soft-wrap chain share an ID. Once the line
    // scrolls into scrollback, the ID stays with it until eviction.
    uint64_t lineIdForAbs(int abs) const;
    int firstAbsOfLine(uint64_t id) const;
    int lastAbsOfLine(uint64_t id) const;
    uint64_t newestLineId() const;
    void inheritLineIdFromAbove(int abs);

    // Plain-text extraction across an abs-row span specified by line IDs.
    // Inclusive endpoints. startCol/endCol clip the first/last line.
    std::string getTextFromLines(uint64_t startLineId, uint64_t endLineId,
                                 int startCol = 0,
                                 int endCol = std::numeric_limits<int>::max()) const;

    // Eviction callback: fires once per dropped line ID after it's removed
    // from scrollback. Used by Terminal to destroy embedded terminals
    // anchored to evicted lines.
    void setOnLineIdEvicted(std::function<void(uint64_t)> cb);

    // Scrollback configuration.
    void setMaxLogicalLines(int n);
    void setMaxTotalCells(int n);

private:
    // --- Visible grid (ring of screenHeight_ rows) ---
    static constexpr int SEG_SHIFT = 6;
    static constexpr int SEG_SIZE  = 1 << SEG_SHIFT;
    static constexpr int SEG_MASK  = SEG_SIZE - 1;

    int cols_ = 0;
    int screenHeight_ = 0;

    int ringCapacity_ = 0;
    int ringHead_ = 0;            // physical slot after last visible row
    std::vector<Cell*> segments_;
    std::vector<std::unordered_map<int, CellExtra>> ringExtras_;

    enum RowFlag : uint8_t {
        Continued = 1 << 0,
        HasWide   = 1 << 1,
    };
    std::vector<uint8_t> rowFlags_;  // indexed by physical ring slot

    // --- Scrollback ---
    LineBuffer scrollback_;

    // Per-visible-row line IDs (size == screenHeight_).
    std::vector<uint64_t> screenLineId_;
    uint64_t nextLineId_ = 1;

    // Dirty (screen-relative).
    std::vector<bool> dirty_;
    bool allDirty_ = true;

    // Lazy padded buffer for historyRow() callers.
    mutable std::vector<Cell> wrapBuffer_;
    mutable std::unordered_map<int, CellExtra> wrapBufferExtras_;
    mutable int wrapBufferRowIdx_ = -1;
    mutable int wrapBufferWidth_ = -1;
    mutable bool wrapBufferContinued_ = false;

    // External eviction listener.
    std::function<void(uint64_t)> onLineIdEvicted_;

    // Helpers
    bool rowFlag(int phys, RowFlag f) const { return (rowFlags_[phys] & f) != 0; }
    void setRowFlag(int phys, RowFlag f, bool v) {
        if (v) rowFlags_[phys] = rowFlags_[phys] | f;
        else   rowFlags_[phys] = rowFlags_[phys] & static_cast<uint8_t>(~f);
    }

    int ringMask() const { return ringCapacity_ - 1; }
    int screenRowToPhysical(int screenRow) const;
    Cell* rowPtr(int physical) const {
        return segments_[physical >> SEG_SHIFT] + (physical & SEG_MASK) * cols_;
    }

    void clearPhysicalRow(int physical);
    void freeSegments();
    void allocSegments(int from, int to);
    static int roundUpPow2(int v);

    // Push a visible row to scrollback (called when the ring advances
    // past it). screenRow is used to look up the row's line ID; phys is
    // the corresponding physical ring slot.
    void pushVisibleRowToScrollback(int screenRow, int phys);

    // Reflow the visible grid for a width or height change. Pushes used
    // visible-grid rows into scrollback, then pops back enough wrapped
    // rows at the new width to fill the new visible grid.
    void resizeReflow(int newCols, int newRows, CursorTrack* cursor);

    // Allocate a fresh visible-grid ring (replaces existing one). Called
    // by resize when cols change. Updates segments_, ringCapacity_,
    // ringHead_, ringExtras_, rowFlags_, dirty_, screenLineId_ to fresh
    // empty state for a (newCols, newRows) grid.
    void resetVisibleGrid(int newCols, int newRows);

    // Populate wrapBuffer_ for a wrapped scrollback row at idx.
    void materializeWrappedRow(int idx) const;

    // Apply scrollback's eviction to fire onLineIdEvicted_ for the
    // visible-grid → scrollback path, even though we don't own it.
    void wireScrollbackEviction();
};
