#pragma once

#include "CellTypes.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Variable-length logical-line storage for terminal scrollback.
//
// One logical line corresponds to one "raw" line — text terminated by a hard
// newline. Display-time wrap is computed on demand at the current screen
// width with a per-(line, width) MRU cache. Resize is a no-op on storage.
//
// The buffer is a deque of fixed-budget arena blocks. Each block packs many
// logical lines into one contiguous Cell array plus a parallel
// cumulativeLengths_ index. Eviction drops oldest blocks (or oldest lines
// within the head block) when bounds are exceeded.
//
// Eviction bounds:
//   - maxLogicalLines: primary, width-independent bound.
//   - maxTotalCells:   backstop against pathological single long lines.

struct LineMeta
{
    enum Eol : uint8_t
    {
        EolHard = 0, // explicit \n; logical line ends here
        EolSoft = 1, // autowrap; logical line continues on next physical row
        EolDwc  = 2, // DWC straddled the right edge; treated like Soft for join
    };

    enum Flag : uint8_t
    {
        HasWide = 1 << 0,
    };

    uint8_t flags  = 0;
    Eol eol        = EolHard;
    bool isPartial = false; // last line of last block; cells may be appended

    uint64_t lineId = 0; // monotonic ID, stable across resize

    // Sparse extras (combining marks, hyperlinks, image refs, embedded ids,
    // underline color), keyed by column within this line.
    std::unordered_map<int, CellExtra> extras;

    // Per-line MRU wrap cache (computed by LogicalLineBlock).
    mutable int cachedWidth       = -1;
    mutable int cachedWrappedRows = 0;
};

class LogicalLineBlock
{
public:
    // Target ~8 KB of cells per block. sizeof(Cell)==12, so 682 ≈ 8 KB.
    static constexpr int kCellCapacity = 682;

    LogicalLineBlock();

    // Append a line (or extend the partial last line if extendsLast == true).
    // Returns false if the cells don't fit; caller should seal this block and
    // open a new one. `extendsLast` is true only when the previous line in this
    // block is currently partial AND the new content is its continuation
    // (EOL_SOFT scroll-out chained to a partial scrollback line).
    bool appendLine(const Cell *cells, int len,
                    LineMeta::Eol eol, bool partial, bool extendsLast,
                    uint64_t lineId, uint8_t flags,
                    const std::unordered_map<int, CellExtra> *extras);

    // Number of currently visible (not dropped-from-head) logical lines.
    int numLines() const { return static_cast<int>(meta_.size()) - firstValidLine_; }

    bool empty() const { return numLines() == 0; }

    // Internal head pointer into meta_/cumulativeLengths_. Exposed so
    // LineBuffer's lineId index can store stable internal indices that
    // survive dropFront() (which advances firstValidLine_ rather than
    // shifting meta_ entries).
    int firstValidLine() const { return firstValidLine_; }

    // Cells in use by visible (not-yet-dropped) lines. For backstop accounting.
    int cellsUsed() const { return static_cast<int>(cells_.size()) - bufferStartOffset_; }

    // Line accessors. `i` is 0..numLines()-1, relative to current head.
    const Cell *lineCells(int i) const;
    int lineLength(int i) const;

    const LineMeta &meta(int i) const { return meta_[firstValidLine_ + i]; }

    LineMeta &mutableMeta(int i);

    uint64_t lineId(int i) const { return meta_[firstValidLine_ + i].lineId; }

    bool lastIsPartial() const;

    // Wrap calculations. MRU per block; per-line MRU lives in LineMeta.
    int numWrappedRows(int width) const;
    int numWrappedRowsForLine(int i, int width) const;

    // Drop the first n lines from this block. Returns true if the block is
    // now empty (caller should remove it from the deque).
    bool dropFront(int n);

    // Drop the last line. Used by popLastLine for resize-grow path.
    void dropLast();

    void invalidateWrapCache();

private:
    std::vector<Cell> cells_;
    std::vector<int> cumulativeLengths_; // [i] = end offset of meta_[i] in cells_
    std::vector<LineMeta> meta_;
    int firstValidLine_    = 0; // head pointer into meta_/cumulativeLengths_
    int bufferStartOffset_ = 0; // start offset of cells for firstValidLine_

    mutable int cachedWidth_       = -1;
    mutable int cachedWrappedRows_ = 0;

    int lineStartOffset(int absLineIdx) const
    {
        return (absLineIdx == firstValidLine_) ? bufferStartOffset_
                                               : cumulativeLengths_[absLineIdx - 1];
    }
};

class LineBuffer
{
public:
    static constexpr int kDefaultMaxLogicalLines = 100000;
    static constexpr int kDefaultMaxTotalCells   = 50'000'000;

    LineBuffer();
    LineBuffer(int maxLogicalLines, int maxTotalCells);

    // Configuration.
    void setMaxLogicalLines(int n);
    void setMaxTotalCells(int n);

    int maxLogicalLines() const { return maxLogicalLines_; }

    int maxTotalCells() const { return maxTotalCells_; }

    // Diagnostic: total number of per-block entries rebuilt by
    // ensureSumCache / ensureLogicalCache since construction. Counts the
    // sum of `nb` across every full rebuild. Steady-state streaming MUST
    // not increase this — that's the regression guard for the prefix-sum
    // moving-base scheme. Width changes / explicit invalidation do trigger
    // rebuilds; those bumps are expected.
    uint64_t cacheRebuildEntries() const { return cacheRebuildEntries_; }

    // Append a row's used cells. The combination (wasContinued, isLastRowSoft)
    // is encoded by the caller into eol + extendsLast:
    //   - extendsLast: this line continues a partial scrollback line.
    //   - eol = EolSoft: the line is still wrapping after this row's cells.
    //   - eol = EolHard: explicit hard break (logical line ends here).
    //   - eol = EolDwc:  DWC at the right edge forced a wrap.
    //
    // `extras` is optional; when non-null its entries are keyed by column
    // *within this row*; appendLine remaps to column-within-the-logical-line
    // by adding the existing line's length when extending.
    //
    // `lineId` is assigned when starting a new line; ignored when extending.
    void appendLine(const Cell *cells, int len,
                    LineMeta::Eol eol, bool partial, bool extendsLast,
                    uint64_t lineId, uint8_t flags,
                    const std::unordered_map<int, CellExtra> *extras);

    // Convenience: append a complete logical line that ends with a hard break.
    void appendHardLine(const Cell *cells, int len,
                        uint64_t lineId, uint8_t flags,
                        const std::unordered_map<int, CellExtra> *extras);

    // Pop the last line off entirely (used when resize grows the visible grid
    // and we need to repopulate visible rows from the bottom of the
    // scrollback).
    struct PoppedLine
    {
        bool ok = false;
        std::vector<Cell> cells;
        LineMeta::Eol eol = LineMeta::EolHard;
        bool wasPartial   = false;
        uint64_t lineId   = 0;
        uint8_t flags     = 0;
        std::unordered_map<int, CellExtra> extras;
    };

    PoppedLine popLastLine();

    // Counts.
    int totalLogicalLines() const { return totalLines_; }

    int totalCells() const { return totalCells_; }

    int blockCount() const { return static_cast<int>(blocks_.size()); }

    // Wrap calculations.
    int numWrappedRows(int width) const;

    // Resolve a wrapped-row index (0..numWrappedRows-1) to a (block, line,
    // offset) position. Returns false on out-of-range.
    struct WrappedLineRef
    {
        int blockIdx          = 0;
        int lineInBlock       = 0;
        int rowOffset         = 0; // start offset within the logical line's cells
        int rowLength         = 0; // length of this wrapped row (≤ width)
        bool isFirstRowOfLine = false;
        bool isLastRowOfLine  = false;
        LineMeta::Eol eol     = LineMeta::EolHard;
    };

    bool wrappedRowAt(int wrappedRow, int width, WrappedLineRef *out) const;

    // Direct cell pointer for the resolved wrapped row.
    const Cell *cellsAt(const WrappedLineRef &ref) const;

    // Convenience: resolve and fetch in one call. *outLen receives row length.
    // Returns nullptr on out-of-range.
    const Cell *wrappedRowCells(int wrappedRow, int width, int *outLen) const;

    // Logical-line iteration.
    const LogicalLineBlock &block(int i) const { return blocks_[i]; }

    LogicalLineBlock &block(int i) { return blocks_[i]; }

    // Last logical line accessors (for the in-progress soft-wrap append
    // chain, and for the visible-grid restore-from-scrollback path).
    bool lastLineIsPartial() const;

    // O(1) lineId of the newest logical line in scrollback. Returns 0 when
    // the buffer is empty. Distinct from lineIdAtLogicalIndex(totalLogicalLines()-1)
    // which goes through the logical-prefix-sum cache; this reads straight
    // from blocks_.back() so it never triggers a cache rebuild — critical for
    // pushVisibleRowToScrollback's extendsLast check which fires on every LF.
    uint64_t lastLineId() const;

    // Line-ID resolution.
    //
    // logicalIndex = 0 means oldest line, totalLogicalLines()-1 = newest.
    uint64_t lineIdAtLogicalIndex(int idx) const;
    // Returns logical-index of the line with the given id, or -1 if not in
    // the buffer (evicted or never existed).
    int logicalIndexOfLineId(uint64_t id) const;

    // O(1) lineId → position lookup via the index. Replaces the prior
    // linear scan in logicalIndexOfLineId / firstAbsOfLine. Returns nullopt
    // when the line was evicted, has never been added, or its block has
    // been popped from the front.
    struct FoundLine
    {
        int blockIdx;        // current deque index of the block
        int externalLineIdx; // 0-based, post-dropFront external index
    };

    std::optional<FoundLine> findLine(uint64_t id) const;

    // Wrapped-row offset to the start of `blockIdx`. Cached via
    // ensureSumCache(width); O(1) on warm cache.
    int numWrappedRowsBeforeBlock(int blockIdx, int width) const;

    // Iteration helpers: resolve logical-index → (block, lineInBlock).
    bool resolveLogicalIndex(int idx, int *blockIdx, int *lineInBlock) const;

    // Plain-text extraction helper: full text of a logical line, no
    // formatting. trims trailing nulls.
    std::string lineText(int logicalIndex) const;

    // Extract a contiguous span of logical lines as plain text, joined with
    // '\n'. start/end are logical-indexes (inclusive). If startCol > 0 the
    // first line is sliced at startCol; if endCol < INT_MAX the last line is
    // truncated at endCol. Out-of-range indexes are clamped.
    std::string textInRange(int startIdx, int endIdx,
                            int startCol = 0, int endCol = -1) const;

    // Wipe everything.
    void clear();

    // Invalidate wrap caches across all blocks. Called on display-width change.
    void invalidateWrapCaches();

    // Eviction callback (fires once per dropped line ID after it's removed).
    void setOnLineIdEvicted(std::function<void(uint64_t)> cb)
    {
        onLineIdEvicted_ = std::move(cb);
    }

private:
    std::deque<LogicalLineBlock> blocks_;
    int maxLogicalLines_;
    int maxTotalCells_;

    int totalLines_ = 0;
    int totalCells_ = 0;

    std::function<void(uint64_t)> onLineIdEvicted_;

    // Width-keyed cumulative wrap-row cache.
    //
    // Invariant: cachedBlockEndCum_[i] = total wrapped rows in *all* blocks
    // ever inserted at indices [0, sumBaseBlocks_ + i] at width
    // cachedSumWidth_ — i.e. it's a prefix sum in an absolute coordinate
    // frame anchored at "everything ever evicted plus what's still live".
    //
    // sumBaseOffset_ = total wrapped rows of *fully evicted* head blocks
    // (and dropped-but-block-not-empty head lines).
    // sumBaseBlocks_ = count of blocks ever popped from the front.
    //
    // To get the "real" prefix sum (rows before block i in the current
    // live deque), use cachedBlockEndCum_[i] - sumBaseOffset_.
    //
    // Live entry count equals blocks_.size(): on pop_front we pop_front the
    // cache too. On new block we push_back. Front-pop is O(1) regardless of
    // total scrollback size — critical for sustained streaming at cap.
    //
    // Validity: cachedSumWidth_ == -1 means cache is fully invalid (rebuild
    // from scratch on next ensureSumCache).
    // Stored entries use int64_t even though per-block contributions are
    // int-sized: at sustained high streaming rates the absolute prefix sum
    // (which includes everything ever evicted) can exceed INT_MAX in a
    // single long session. ~2.1B logical lines = ~6 hours at 100k lines/s,
    // which is within reach of compile-log floods at the user's no-cap
    // setting. int64_t is overkill for live block count but the right type
    // for the absolute frame.
    mutable int cachedSumWidth_             = -1;
    mutable int64_t cachedTotalWrappedRows_ = 0;
    mutable std::deque<int64_t> cachedBlockEndCum_;
    mutable int64_t sumBaseOffset_ = 0;

    // Width-independent cumulative logical-line-count cache. Same scheme:
    // cachedBlockEndLogical_[i] - logicalBaseOffset_ = lines before block i.
    mutable bool cachedLogicalValid_ = false;
    mutable std::deque<int64_t> cachedBlockEndLogical_;
    mutable int64_t logicalBaseOffset_ = 0;

    // Diagnostic counter; see cacheRebuildEntries().
    mutable uint64_t cacheRebuildEntries_ = 0;

    // O(1) lineId → location index. Each entry stores the block's *stable*
    // seq plus the line's *internal* meta_-array index, so the entry survives
    // dropFront() (which advances firstValidLine_ but doesn't shift meta_)
    // and pop_front() (handled by adjusting firstBlockSeq_, see below).
    // Updated on every appendLine that creates a new line, on popLastLine,
    // and on enforceLimits eviction; cleared on clear().
    struct LineLocation
    {
        uint64_t blockSeq;
        int internalLineIdx;
    };

    std::unordered_map<uint64_t, LineLocation> lineIdIndex_;
    // Stable, monotonic seq for blocks_.front(). Increments by 1 every time
    // a head block is popped. blocks_[i] has seq == firstBlockSeq_ + i, so
    // we can convert an index entry's stored seq back to the current deque
    // index in O(1) without touching the entries on eviction.
    uint64_t firstBlockSeq_ = 0;

    void ensureSumCache(int width) const;
    void ensureLogicalCache() const;

    void invalidateSumCache()
    {
        cachedSumWidth_     = -1;
        cachedLogicalValid_ = false;
        cachedBlockEndCum_.clear();
        cachedBlockEndLogical_.clear();
        sumBaseOffset_     = 0;
        logicalBaseOffset_ = 0;
    }

    // Called after a back-of-deque mutation (append / pop-last) and after
    // enforceLimits has run. Decides whether the caches can be updated
    // incrementally (cheap, common case) or must be invalidated (eviction
    // happened, or a cache wasn't valid to begin with).
    //
    // preBlockCount is blocks_.size() before any mutation.
    // evicted is true iff enforceLimits dropped any line from the front.
    void afterBackBlockMutation(int preBlockCount, bool evicted);

    void enforceLimits();
};
