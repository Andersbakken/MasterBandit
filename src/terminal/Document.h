#pragma once

#include "IGrid.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

class Document : public IGrid {
public:
    Document();
    Document(int cols, int screenHeight, int tier1Capacity = 4096, int maxArchiveRows = 100000);
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // --- IGrid interface (screen-relative coordinates) ---
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

    // --- History/scrollback API ---
    int historySize() const;
    int archiveSize() const { return static_cast<int>(archive_.size()); }
    const Cell* historyRow(int idx) const;  // idx 0 = oldest
    const std::unordered_map<int, CellExtra>* historyExtras(int idx) const;
    void clearHistory();

    // --- Viewport support ---
    // Returned pointers alias ring-buffer storage: valid only while the
    // owning TerminalEmulator's mutex is held and until the next mutation
    // (resize invalidates the pointer; scrollUp / clearPhysicalRow reuse
    // the slot so content at the same address changes). External callers
    // should go through TerminalEmulator::copyViewportRow instead.
    const Cell* viewportRow(int viewRow, int viewportOffset) const;
    const std::unordered_map<int, CellExtra>* viewportExtras(int viewRow, int viewportOffset) const;

    // --- Resize ---
    struct CursorTrack {
        int srcX, srcY;   // input: cursor position as absolute row index (archive + history + screen)
        int dstX, dstY;   // output: cursor position in destination absolute row index
    };
    void resize(int newCols, int newRows, CursorTrack* cursor = nullptr);

    // --- Per-row continuation flag (soft wrap tracking) ---
    bool isRowContinued(int screenRow) const;
    void setRowContinued(int screenRow, bool v);
    bool isHistoryRowContinued(int idx) const; // idx 0 = oldest (archive + tier-1)

    // --- Logical-line IDs ---
    //
    // Each logical line (a soft-wrap chain of physical rows terminated by a
    // hard newline) gets a monotonic 64-bit identifier at write time. IDs
    // survive reflow (content is unchanged; only the number of physical rows
    // representing it may change) and stay with the line until its last
    // surviving row evicts past the archive cap. Reuse of IDs never happens.
    //
    // Storage: `rowLineId_` is a flat deque indexed by current abs row. Each
    // entry is the ID of the logical line that row belongs to. Entries are
    // non-decreasing, so firstAbsOfLine / lastAbsOfLine use binary search.

    // Line id for the logical line containing this abs row. 0 if abs is
    // out of range.
    uint64_t lineIdForAbs(int abs) const;
    // First abs row of the given logical line, or -1 if the line has no
    // surviving physical rows.
    int firstAbsOfLine(uint64_t id) const;
    // Last abs row of the given logical line, or -1 if not found.
    int lastAbsOfLine(uint64_t id) const;
    // Most recently minted line id (the id of the row currently at max abs).
    uint64_t newestLineId() const;

    // Mark a physical row as the autowrap continuation of the row above —
    // copies the predecessor's line id so the entire logical line shares one
    // id. Called by TerminalEmulator::advanceCursorToNewLine immediately
    // after the cursor lands on the new row.
    void inheritLineIdFromAbove(int abs);

    // Extract plain UTF-8 text from a logical-line-id range (inclusive).
    // startLineId maps to firstAbsOfLine, endLineId to lastAbsOfLine — a
    // multi-row wrapped line is covered end-to-end. startCol/endCol bound
    // the first/last row's columns; pass 0 and INT_MAX for full rows.
    // Lines evicted past the archive cap are skipped silently.
    std::string getTextFromLines(uint64_t startLineId, uint64_t endLineId,
                                 int startCol = 0,
                                 int endCol = std::numeric_limits<int>::max()) const;

    // Called exactly once per line id as it evicts past the archive cap.
    // Fires from inside evictToArchive() after rowLineId_.pop_front() — the
    // id is no longer resolvable through firstAbsOfLine/lastAbsOfLine when
    // the callback runs. The owning Terminal uses this to destroy any
    // embedded Terminal anchored to the evicted line. May be null.
    void setOnLineIdEvicted(std::function<void(uint64_t)> cb) { onLineIdEvicted_ = std::move(cb); }

private:
    // Segmented ring buffer: each segment holds SEG_SIZE rows of cols_ cells.
    // Segments are allocated without construction; cells are written before read
    // (either by growRing/resize copies or by clearPhysicalRow before first use).
    static constexpr int SEG_SHIFT = 6;              // log2(rows per segment)
    static constexpr int SEG_SIZE  = 1 << SEG_SHIFT; // 64 rows per segment
    static constexpr int SEG_MASK  = SEG_SIZE - 1;   // 63

    int cols_ = 0;
    int screenHeight_ = 0;

    // Ring buffer: holds history (tier 1) + screen rows
    int ringCapacity_ = 0;   // power of 2, always a multiple of SEG_SIZE
    int ringHead_ = 0;       // physical slot after last screen row
    int historyCount_ = 0;   // tier-1 history rows in ring
    std::vector<Cell*> segments_; // segments_[i] → SEG_SIZE * cols_ uninitialized Cells
    std::vector<std::unordered_map<int, CellExtra>> ringExtras_;
    std::vector<bool> continued_;  // per physical ring slot: row was soft-wrapped

    // Pointer to the first cell of physical ring slot p.
    Cell* rowPtr(int p) const {
        return segments_[p >> SEG_SHIFT] + (p & SEG_MASK) * cols_;
    }

    // Logical-line ids, indexed by current abs row. Flat deque (not circular).
    // Length always equals archive_.size() + historyCount_ + screenHeight_.
    // Non-decreasing because IDs mint in write order which matches abs order.
    std::deque<uint64_t> rowLineId_;
    uint64_t nextLineId_ = 1;
    // Fired from evictToArchive() when a line id leaves the archive cap.
    // See setOnLineIdEvicted docs above.
    std::function<void(uint64_t)> onLineIdEvicted_;
    // Mint a fresh ID (never reuses).
    uint64_t mintLineId() { return nextLineId_++; }

    // Dirty tracking (screen-relative)
    std::vector<bool> dirty_;
    bool allDirty_ = true;

    // Tier 2: compressed archive
    struct ArchivedRow { std::string data; bool continued = false; };
    std::deque<ArchivedRow> archive_;
    int maxArchiveRows_ = 0;
    int tier1Capacity_ = 0;       // max history rows before eviction to archive
    mutable std::vector<Cell> parseBuffer_;
    mutable std::unordered_map<int, CellExtra> parseBufferExtras_;

    // Internal helpers
    int ringMask() const { return ringCapacity_ - 1; }
    int screenRowToPhysical(int screenRow) const;
    int historyTier1ToPhysical(int tier1Idx) const;
    void evictToArchive();
    void clearPhysicalRow(int physical);
    void growRing();
    // General grow used by resize(): linearizes existing circular data into
    // new segments. Slower than growRing() but handles arbitrary ringHead_.
    void growRingGeneral(int newCap);
    // Free all segments and clear the vector.
    void freeSegments();
    // Allocate n segments of SEG_SIZE * cols_ cells each, appending to segments_.
    void allocSegments(int from, int to);

    static int roundUpPow2(int v);
    static std::string serializeRow(const Cell* cells, int cols,
                                    const std::unordered_map<int, CellExtra>* extras = nullptr);
    void parseArchivedRow(const ArchivedRow& row) const;
};
