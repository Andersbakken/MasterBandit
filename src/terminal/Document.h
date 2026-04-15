#pragma once

#include "IGrid.h"
#include <deque>
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

    // --- Stable monotonic row ids ---
    //
    // Each row in the structure has a logical id that doesn't change as the row
    // scrolls through archive / tier-1 / screen. IDs are assigned implicitly by
    // absolute position: `rowIdForAbs(abs) = rowIdBase_ + abs`. Archive-head
    // eviction bumps `rowIdBase_` so that remaining rows' ids stay constant
    // while their abs-row numbers shift down. Reflow on resize does NOT preserve
    // ids (rows split/merge and logical identity breaks); callers treat ids
    // from before a resize as best-effort.
    uint64_t rowIdForAbs(int abs) const;
    // Returns -1 if the row has been evicted from archive or is otherwise gone.
    int absForRowId(uint64_t id) const;

    // Extract plain UTF-8 text from a stable row-id range (inclusive on both ends).
    // startCol/endCol bound the columns on the first/last row; pass 0 and INT_MAX
    // for full rows. Rows evicted past the archive cap are skipped silently.
    std::string getTextFromRows(uint64_t startRowId, uint64_t endRowId,
                                int startCol = 0,
                                int endCol = std::numeric_limits<int>::max()) const;

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

    // Row-id base: the id of the row currently at abs-row 0. Increments when
    // the oldest archive row evicts (so remaining rows' ids stay stable).
    uint64_t rowIdBase_ = 0;

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
