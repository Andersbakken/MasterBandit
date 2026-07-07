#include "LineBuffer.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

// =========================================================================
// LogicalLineBlock
// =========================================================================

LogicalLineBlock::LogicalLineBlock()
{
    cells_.reserve(kCellCapacity);
    cumulativeLengths_.reserve(64);
    meta_.reserve(64);
}

bool LogicalLineBlock::appendLine(const Cell *cells, int len,
                                  LineMeta::Eol eol, bool partial, bool extendsLast,
                                  uint64_t lineId, uint8_t flags,
                                  const std::unordered_map<int, CellExtra> *extras)
{
    if (len < 0) {
        return false;
    }

    // For "extend last partial line", the previous line's metadata is mutated
    // and cells are appended.
    if (extendsLast) {
        if (meta_.empty() || firstValidLine_ >= static_cast<int>(meta_.size())) {
            // Cannot extend nothing — caller must seal block and retry without
            // extendsLast.
            return false;
        }
        const int lastIdx = static_cast<int>(meta_.size()) - 1;
        if (!meta_[lastIdx].isPartial) {
            return false;
        }

        // Capacity check: do we have room for `len` more cells?
        if (static_cast<int>(cells_.size()) + len > kCellCapacity && len > 0) {
            // Allow some overflow but not unbounded.
            if (static_cast<int>(cells_.size()) + len > kCellCapacity * 2) {
                return false;
            }
        }
        const int currentEnd = cumulativeLengths_[lastIdx];
        const int prevStart  = (lastIdx == firstValidLine_) ? bufferStartOffset_
                                                            : cumulativeLengths_[lastIdx - 1];
        const int currentLen = currentEnd - prevStart;

        if (len > 0) {
            cells_.insert(cells_.end(), cells, cells + len);
        }
        cumulativeLengths_[lastIdx] = static_cast<int>(cells_.size());

        meta_[lastIdx].eol       = eol;
        meta_[lastIdx].isPartial = partial;
        meta_[lastIdx].flags |= flags;
        if (extras) {
            for (const auto &[col, ex] : *extras) {
                if (col < 0 || col >= len) {
                    continue;
                }
                meta_[lastIdx].extras[col + currentLen] = ex;
            }
        }
        meta_[lastIdx].cachedWidth = -1;
        cachedWidth_               = -1;
        return true;
    }

    // New line. Check capacity.
    if (static_cast<int>(cells_.size()) + len > kCellCapacity && !empty()) {
        return false;
    }

    if (len > 0) {
        cells_.insert(cells_.end(), cells, cells + len);
    }
    cumulativeLengths_.push_back(static_cast<int>(cells_.size()));

    LineMeta m;
    m.flags     = flags;
    m.eol       = eol;
    m.isPartial = partial;
    m.lineId    = lineId;
    if (extras) {
        for (const auto &[col, ex] : *extras) {
            if (col < 0 || col >= len) {
                continue;
            }
            m.extras[col] = ex;
        }
    }
    meta_.push_back(std::move(m));
    cachedWidth_ = -1;
    return true;
}

const Cell *LogicalLineBlock::lineCells(int i) const
{
    const int abs   = firstValidLine_ + i;
    const int start = lineStartOffset(abs);
    return cells_.data() + start;
}

int LogicalLineBlock::lineLength(int i) const
{
    const int abs   = firstValidLine_ + i;
    const int start = lineStartOffset(abs);
    const int end   = cumulativeLengths_[abs];
    return end - start;
}

LineMeta &LogicalLineBlock::mutableMeta(int i)
{
    cachedWidth_  = -1;
    LineMeta &m   = meta_[firstValidLine_ + i];
    m.cachedWidth = -1;
    return m;
}

bool LogicalLineBlock::lastIsPartial() const
{
    if (empty()) {
        return false;
    }
    return meta_.back().isPartial;
}

// Number of wrapped rows for one line of `len` cells at `width`.
// HasWide is currently a hint; the wrap calculation is plain integer for
// non-wide content and identical (no DWC special-case yet) for wide content
// because Cells already encode wide+spacer pairs as two cells. A line of N
// cells at width W produces ⌈N / W⌉ rows, with at least 1.
static int wrappedRowsFor(int len, int width)
{
    if (width <= 0) {
        return 1;
    }
    if (len <= 0) {
        return 1;
    }
    return (len + width - 1) / width;
}

int LogicalLineBlock::numWrappedRowsForLine(int i, int width) const
{
    const LineMeta &m = meta(i);
    if (m.cachedWidth == width) {
        return m.cachedWrappedRows;
    }
    const int len       = lineLength(i);
    int rows            = wrappedRowsFor(len, width);
    m.cachedWidth       = width;
    m.cachedWrappedRows = rows;
    return rows;
}

int LogicalLineBlock::numWrappedRows(int width) const
{
    if (cachedWidth_ == width) {
        return cachedWrappedRows_;
    }
    int total = 0;
    for (int i = 0; i < numLines(); ++i) {
        total += numWrappedRowsForLine(i, width);
    }
    cachedWidth_       = width;
    cachedWrappedRows_ = total;
    return total;
}

void LogicalLineBlock::invalidateWrapCache()
{
    cachedWidth_ = -1;
    for (auto &m : meta_) {
        m.cachedWidth = -1;
    }
}

bool LogicalLineBlock::dropFront(int n)
{
    if (n <= 0) {
        return empty();
    }
    const int avail = numLines();
    n               = std::min(n, avail);
    firstValidLine_ += n;
    if (firstValidLine_ < static_cast<int>(meta_.size())) {
        bufferStartOffset_ = cumulativeLengths_[firstValidLine_ - 1];
    } else {
        // Block is now empty.
        cells_.clear();
        cumulativeLengths_.clear();
        meta_.clear();
        firstValidLine_    = 0;
        bufferStartOffset_ = 0;
    }
    cachedWidth_ = -1;
    return empty();
}

void LogicalLineBlock::dropLast()
{
    if (empty()) {
        return;
    }
    meta_.pop_back();
    cumulativeLengths_.pop_back();
    if (cumulativeLengths_.empty()) {
        cells_.clear();
        bufferStartOffset_ = 0;
    } else {
        cells_.resize(cumulativeLengths_.back());
    }
    cachedWidth_ = -1;
}

// =========================================================================
// LineBuffer
// =========================================================================

LineBuffer::LineBuffer()
    : maxLogicalLines_(kDefaultMaxLogicalLines)
    , maxTotalCells_(kDefaultMaxTotalCells)
{
}

LineBuffer::LineBuffer(int maxLogicalLines, int maxTotalCells)
    : maxLogicalLines_(maxLogicalLines)
    , maxTotalCells_(maxTotalCells)
{
}

void LineBuffer::setMaxLogicalLines(int n)
{
    maxLogicalLines_ = n;
    enforceLimits();
    invalidateSumCache();
}

void LineBuffer::setMaxTotalCells(int n)
{
    maxTotalCells_ = n;
    enforceLimits();
    invalidateSumCache();
}

void LineBuffer::appendLine(const Cell *cells, int len,
                            LineMeta::Eol eol, bool partial, bool extendsLast,
                            uint64_t lineId, uint8_t flags,
                            const std::unordered_map<int, CellExtra> *extras)
{
    // Snapshot pre-append state so afterBackBlockMutation can either update
    // the sum cache incrementally (the common case) or invalidate it
    // (eviction happened, cache was already cold, etc.).
    const int preBlockCount = static_cast<int>(blocks_.size());

    // Try to append to the last block. If it fails (capacity, or the
    // extendsLast precondition isn't met because the last line is in a
    // different block), open a new block.
    bool appended = false;
    if (!blocks_.empty()) {
        appended = blocks_.back().appendLine(cells, len, eol, partial, extendsLast, lineId, flags, extras);
    }
    if (!appended) {
        // The last block rejected the append. When extendsLast is true this
        // means either the block had no partial last line (caller invariant
        // recovered as a fresh line) or capacity overflow on extension (seal
        // and restart the continuation as a new partial line in a fresh
        // block, keeping the same line ID). Both cases, and the ordinary
        // extendsLast=false case, open a new block and append as a new line.
        blocks_.emplace_back();
        const bool ok = blocks_.back().appendLine(cells, len, eol, partial, false, lineId, flags, extras);
        (void)ok;
        assert(ok);
        ++totalLines_;
    } else if (!extendsLast) {
        ++totalLines_;
    }

    // Index the line. extendsLast on a successful append doesn't add a new
    // line — same lineId, same internal slot — so the existing index entry
    // remains correct; skip in that case. All other paths (new line in the
    // existing block, sealed-then-restart on overflow, fresh new block)
    // produce a new meta_ entry at internal index meta_.size()-1, which is
    // firstValidLine() + numLines() - 1 for the back block.
    //
    // try_emplace, not assign: when a logical line is too long for one block
    // we seal and restart in a fresh block with the SAME lineId. The index
    // must keep pointing at the FIRST block so firstAbsOfLine returns the
    // line's actual first row; overwriting on each continuation made
    // firstAbsOfLine report the row where the last continuation starts.
    if (!(appended && extendsLast)) {
        const int blockIdx    = static_cast<int>(blocks_.size()) - 1;
        const auto &back      = blocks_.back();
        const int internalIdx = back.firstValidLine() + back.numLines() - 1;
        lineIdIndex_.try_emplace(lineId, LineLocation { firstBlockSeq_ + static_cast<uint64_t>(blockIdx), internalIdx });
    }

    totalCells_ += len;

    // enforceLimits may evict from the front. Any eviction shifts every
    // surviving prefix sum, so we have to fall back to full invalidation in
    // that case. Detect it by comparing line counts across the call —
    // popping just one line from the head block still moves the prefix sums
    // even though blocks_.size() may not change.
    const int linesBeforeEvict = totalLines_;
    enforceLimits();
    const bool evicted = totalLines_ < linesBeforeEvict;

    afterBackBlockMutation(preBlockCount, evicted);
}

void LineBuffer::appendHardLine(const Cell *cells, int len,
                                uint64_t lineId, uint8_t flags,
                                const std::unordered_map<int, CellExtra> *extras)
{
    appendLine(cells, len, LineMeta::EolHard, /*partial*/ false, /*extendsLast*/ false, lineId, flags, extras);
}

LineBuffer::PoppedLine LineBuffer::popLastLine()
{
    PoppedLine result;
    if (blocks_.empty()) {
        return result;
    }
    LogicalLineBlock &last = blocks_.back();
    if (last.empty()) {
        blocks_.pop_back();
        return popLastLine();
    }
    const int preBlockCount = static_cast<int>(blocks_.size());

    const int idx = last.numLines() - 1;
    const Cell *p = last.lineCells(idx);
    const int len = last.lineLength(idx);
    result.cells.assign(p, p + len);
    const LineMeta &m = last.meta(idx);
    result.eol        = m.eol;
    result.wasPartial = m.isPartial;
    result.lineId     = m.lineId;
    result.flags      = m.flags;
    result.extras     = m.extras;
    result.ok         = true;

    last.dropLast();
    totalCells_ -= len;
    --totalLines_;
    lineIdIndex_.erase(result.lineId);
    if (last.empty()) {
        // Pop the now-empty trailing block. Its seq is firstBlockSeq_+blocks_.size()-1
        // and is never reused — that's fine, no remaining index entries
        // reference it (we just erased the only one).
        blocks_.pop_back();
    }
    // popLastLine only ever mutates the back block (or removes it). No
    // front-side eviction. afterBackBlockMutation handles the
    // size-decrease case (truncate prefix arrays).
    afterBackBlockMutation(preBlockCount, /*evicted*/ false);
    return result;
}

// Cache scheme overview
// ---------------------
// Both prefix-sum caches use the SAME scheme: cachedBlockEndCum_[i] (or
// cachedBlockEndLogical_[i]) stores the running prefix sum in an absolute
// frame that includes everything ever evicted from the front. The "live"
// prefix sum for block i is `cachedBlockEndCum_[i] - sumBaseOffset_`.
//
// Front-eviction is therefore O(1): pop_front the cache, add the popped
// entry's contribution to the base offset, and every surviving cache entry
// continues to point at the correct logical value via subtraction.
//
// Back-block mutations (append) update only the back entry: O(1).
//
// Width change forces a full rebuild of the wrap cache only. That rebuild
// resets sumBaseOffset_ to 0 because we're choosing a new coordinate frame.

void LineBuffer::ensureSumCache(int width) const
{
    const size_t nb = blocks_.size();
    if (cachedSumWidth_ == width && cachedBlockEndCum_.size() == nb) {
#ifndef NDEBUG
        // Paranoia: in debug builds, verify the live prefix sums against a
        // ground-truth walk every time we hit the warm path. If incremental
        // maintenance ever drifts (eviction failed to update the offset,
        // append failed to bump the back entry, etc.), this fires at the
        // next query rather than corrupting downstream rendering.
        int64_t verifyTotal = 0;
        for (size_t i = 0; i < nb; ++i) {
            verifyTotal += blocks_[i].numWrappedRows(width);
            const int64_t live = cachedBlockEndCum_[i] - sumBaseOffset_;
            assert(live == verifyTotal &&
                   "LineBuffer sum cache out of sync (wrapped prefix)");
        }
        assert(cachedTotalWrappedRows_ == verifyTotal &&
               "LineBuffer sum cache out of sync (total wrapped)");
#endif
        return;
    }

    // Width changed or cache was invalidated: full rebuild, new frame.
    cachedBlockEndCum_.clear();
    sumBaseOffset_ = 0;
    int64_t total  = 0;
    for (size_t i = 0; i < nb; ++i) {
        total += blocks_[i].numWrappedRows(width);
        cachedBlockEndCum_.push_back(total);
    }
    cachedTotalWrappedRows_ = total;
    cachedSumWidth_         = width;
    cacheRebuildEntries_ += nb;
}

void LineBuffer::ensureLogicalCache() const
{
    const size_t nb = blocks_.size();
    if (cachedLogicalValid_ && cachedBlockEndLogical_.size() == nb) {
#ifndef NDEBUG
        int64_t verifyTotal = 0;
        for (size_t i = 0; i < nb; ++i) {
            verifyTotal += blocks_[i].numLines();
            const int64_t live = cachedBlockEndLogical_[i] - logicalBaseOffset_;
            assert(live == verifyTotal &&
                   "LineBuffer logical cache out of sync");
        }
#endif
        return;
    }

    cachedBlockEndLogical_.clear();
    logicalBaseOffset_ = 0;
    int64_t total      = 0;
    for (size_t i = 0; i < nb; ++i) {
        total += blocks_[i].numLines();
        cachedBlockEndLogical_.push_back(total);
    }
    cachedLogicalValid_ = true;
    cacheRebuildEntries_ += nb;
}

void LineBuffer::afterBackBlockMutation(int preBlockCount, bool /*evicted*/)
{
    // Eviction maintenance is done inline in enforceLimits, not here. By
    // the time this runs, both caches are already consistent with respect
    // to any front-pops that happened. Our job is only to reconcile
    // back-side mutations: in-place back-block grow, or one or more new
    // blocks pushed at the back.
    //
    // Note preBlockCount predates BOTH the back-side append AND any
    // front-side eviction. To know how many blocks were popped from the
    // front we'd have to track it separately — but we don't need to,
    // because the cache deques have already been pop_front'd by
    // enforceLimits. We only care about back-side reconciliation here.

    const size_t nb = blocks_.size();

    // Wrap-rows cache.
    if (cachedSumWidth_ != -1) {
        // If the cache deque is shorter than blocks_, new blocks were
        // pushed at the back since the last reconcile. Extend.
        while (cachedBlockEndCum_.size() < nb) {
            const size_t i     = cachedBlockEndCum_.size();
            const int64_t prev = cachedBlockEndCum_.empty()
                ? sumBaseOffset_
                : cachedBlockEndCum_.back();
            cachedBlockEndCum_.push_back(prev + blocks_[i].numWrappedRows(cachedSumWidth_));
        }
        // If the cache deque is longer than blocks_, the back was
        // truncated (popLastLine emptied the trailing block(s)).
        while (cachedBlockEndCum_.size() > nb) {
            cachedBlockEndCum_.pop_back();
        }
        // The back block itself may have grown/shrunk in place. Recompute
        // its entry from the previous one.
        if (!cachedBlockEndCum_.empty()) {
            const size_t last  = cachedBlockEndCum_.size() - 1;
            const int64_t prev = (last == 0) ? sumBaseOffset_ : cachedBlockEndCum_[last - 1];
            cachedBlockEndCum_[last] =
                prev + blocks_[last].numWrappedRows(cachedSumWidth_);
        }
        cachedTotalWrappedRows_ =
            cachedBlockEndCum_.empty()
            ? 0
            : (cachedBlockEndCum_.back() - sumBaseOffset_);
    }

    // Logical-count cache: same shape, width-independent.
    if (cachedLogicalValid_) {
        while (cachedBlockEndLogical_.size() < nb) {
            const size_t i     = cachedBlockEndLogical_.size();
            const int64_t prev = cachedBlockEndLogical_.empty()
                ? logicalBaseOffset_
                : cachedBlockEndLogical_.back();
            cachedBlockEndLogical_.push_back(prev + blocks_[i].numLines());
        }
        while (cachedBlockEndLogical_.size() > nb) {
            cachedBlockEndLogical_.pop_back();
        }
        if (!cachedBlockEndLogical_.empty()) {
            const size_t last            = cachedBlockEndLogical_.size() - 1;
            const int64_t prev           = (last == 0) ? logicalBaseOffset_ : cachedBlockEndLogical_[last - 1];
            cachedBlockEndLogical_[last] = prev + blocks_[last].numLines();
        }
    }
    (void)preBlockCount;
}

int LineBuffer::numWrappedRows(int width) const
{
    if (width <= 0) {
        return 0;
    }
    ensureSumCache(width);
    // Live total is bounded by maxLogicalLines × max wrap factor —
    // always fits in int; assert in debug builds.
    assert(cachedTotalWrappedRows_ <= std::numeric_limits<int>::max() &&
           "live wrapped-row count overflowed int");
    return static_cast<int>(cachedTotalWrappedRows_);
}

bool LineBuffer::wrappedRowAt(int wrappedRow, int width, WrappedLineRef *out) const
{
    if (wrappedRow < 0 || width <= 0) {
        return false;
    }
    ensureSumCache(width);
    if (static_cast<int64_t>(wrappedRow) >= cachedTotalWrappedRows_) {
        return false;
    }

    // Binary search: first block whose live cumulative end > wrappedRow.
    // Live values are stored values minus sumBaseOffset_. Add the offset
    // to the search key (in int64 space) instead of subtracting from every
    // entry.
    const int64_t searchKey = static_cast<int64_t>(wrappedRow) + sumBaseOffset_;
    auto it                 = std::upper_bound(cachedBlockEndCum_.begin(),
                               cachedBlockEndCum_.end(),
                               searchKey);
    const int bi            = static_cast<int>(it - cachedBlockEndCum_.begin());
    const int64_t prevCum   = (bi == 0) ? sumBaseOffset_ : cachedBlockEndCum_[bi - 1];
    int localRem            = static_cast<int>(static_cast<int64_t>(wrappedRow) - (prevCum - sumBaseOffset_));

    const auto &b = blocks_[bi];
    for (int li = 0; li < b.numLines(); ++li) {
        const int rows = b.numWrappedRowsForLine(li, width);
        if (localRem < rows) {
            const int len       = b.lineLength(li);
            const int rowOffset = localRem * width;
            int rowLen          = std::min(width, len - rowOffset);
            if (rowLen < 0) {
                rowLen = 0;
            }
            out->blockIdx         = bi;
            out->lineInBlock      = li;
            out->rowOffset        = rowOffset;
            out->rowLength        = rowLen;
            out->isFirstRowOfLine = (localRem == 0);
            out->isLastRowOfLine  = (localRem == rows - 1);
            out->eol              = out->isLastRowOfLine ? b.meta(li).eol : LineMeta::EolSoft;
            return true;
        }
        localRem -= rows;
    }
    return false;
}

const Cell *LineBuffer::cellsAt(const WrappedLineRef &ref) const
{
    return blocks_[ref.blockIdx].lineCells(ref.lineInBlock) + ref.rowOffset;
}

const Cell *LineBuffer::wrappedRowCells(int wrappedRow, int width, int *outLen) const
{
    WrappedLineRef ref;
    if (!wrappedRowAt(wrappedRow, width, &ref)) {
        return nullptr;
    }
    if (outLen) {
        *outLen = ref.rowLength;
    }
    return cellsAt(ref);
}

bool LineBuffer::lastLineIsPartial() const
{
    if (blocks_.empty()) {
        return false;
    }
    return blocks_.back().lastIsPartial();
}

uint64_t LineBuffer::lastLineId() const
{
    if (blocks_.empty()) {
        return 0;
    }
    const auto &b = blocks_.back();
    const int n   = b.numLines();
    if (n <= 0) {
        return 0;
    }
    return b.lineId(n - 1);
}

uint64_t LineBuffer::lineIdAtLogicalIndex(int idx) const
{
    int blockIdx = 0, lineInBlock = 0;
    if (!resolveLogicalIndex(idx, &blockIdx, &lineInBlock)) {
        return 0;
    }
    return blocks_[blockIdx].lineId(lineInBlock);
}

int LineBuffer::logicalIndexOfLineId(uint64_t id) const
{
    auto loc = findLine(id);
    if (!loc) {
        return -1;
    }
    // Sum numLines() over preceding blocks. blockCount is small (typically
    // O(scrollback / 64)); per-block size is O(1) (vector::size). For real
    // performance-sensitive callers, prefer findLine + numWrappedRowsBeforeBlock.
    int base = 0;
    for (int bi = 0; bi < loc->blockIdx; ++bi) {
        base += blocks_[bi].numLines();
    }
    return base + loc->externalLineIdx;
}

std::optional<LineBuffer::FoundLine> LineBuffer::findLine(uint64_t id) const
{
    if (id == 0) {
        return std::nullopt;
    }
    auto it = lineIdIndex_.find(id);
    if (it == lineIdIndex_.end()) {
        return std::nullopt;
    }
    const int blockIdx = static_cast<int>(it->second.blockSeq - firstBlockSeq_);
    if (blockIdx < 0 || blockIdx >= static_cast<int>(blocks_.size())) {
        return std::nullopt;
    }
    const auto &b = blocks_[blockIdx];
    const int ext = it->second.internalLineIdx - b.firstValidLine();
    if (ext < 0 || ext >= b.numLines()) {
        return std::nullopt;
    }
    return FoundLine { blockIdx, ext };
}

int LineBuffer::numWrappedRowsBeforeBlock(int blockIdx, int width) const
{
    if (width <= 0 || blockIdx <= 0) {
        return 0;
    }
    ensureSumCache(width);
    const int last = std::min(blockIdx - 1,
                              static_cast<int>(cachedBlockEndCum_.size()) - 1);
    if (last < 0) {
        return 0;
    }
    const int64_t live = cachedBlockEndCum_[last] - sumBaseOffset_;
    assert(live <= std::numeric_limits<int>::max() &&
           "live wrapped-row prefix overflowed int");
    return static_cast<int>(live);
}

bool LineBuffer::resolveLogicalIndex(int idx, int *blockIdx, int *lineInBlock) const
{
    if (idx < 0 || blocks_.empty()) {
        return false;
    }
    ensureLogicalCache();
    // Binary-search the cumulative logical-line-count prefix array in the
    // shifted (int64) frame: stored values include logicalBaseOffset_.
    const int64_t searchKey = static_cast<int64_t>(idx) + logicalBaseOffset_;
    auto it                 = std::upper_bound(cachedBlockEndLogical_.begin(),
                               cachedBlockEndLogical_.end(),
                               searchKey);
    if (it == cachedBlockEndLogical_.end()) {
        return false; // idx >= total lines
    }
    const int bi          = static_cast<int>(it - cachedBlockEndLogical_.begin());
    const int64_t prevLog = (bi == 0) ? logicalBaseOffset_ : *(it - 1);
    *blockIdx             = bi;
    *lineInBlock          = static_cast<int>(static_cast<int64_t>(idx) - (prevLog - logicalBaseOffset_));
    return true;
}

std::string LineBuffer::lineText(int idx) const
{
    int bi, li;
    if (!resolveLogicalIndex(idx, &bi, &li)) {
        return {};
    }
    const Cell *p = blocks_[bi].lineCells(li);
    const int len = blocks_[bi].lineLength(li);
    std::string out;
    out.reserve(len);
    int trim = len;
    while (trim > 0 && p[trim - 1].wc == 0) {
        --trim;
    }
    appendCellsAsUtf8(out, p, 0, trim);
    return out;
}

std::string LineBuffer::textInRange(int startIdx, int endIdx,
                                    int startCol, int endCol) const
{
    if (startIdx < 0) {
        startIdx = 0;
    }
    if (endIdx >= totalLines_) {
        endIdx = totalLines_ - 1;
    }
    if (startIdx > endIdx) {
        return {};
    }
    if (endCol < 0) {
        endCol = std::numeric_limits<int>::max();
    }
    std::string out;
    for (int idx = startIdx; idx <= endIdx; ++idx) {
        int bi, li;
        if (!resolveLogicalIndex(idx, &bi, &li)) {
            continue;
        }
        const Cell *p  = blocks_[bi].lineCells(li);
        const int len  = blocks_[bi].lineLength(li);
        const int from = (idx == startIdx) ? std::max(0, startCol) : 0;
        int to         = (idx == endIdx) ? std::min(len, endCol) : len;
        while (to > from && p[to - 1].wc == 0) {
            --to;
        }
        appendCellsAsUtf8(out, p, from, to);
        if (idx < endIdx) {
            out += '\n';
        }
    }
    return out;
}

void LineBuffer::clear()
{
    blocks_.clear();
    totalLines_ = 0;
    totalCells_ = 0;
    invalidateSumCache();
    lineIdIndex_.clear();
    // firstBlockSeq_ left as-is. Blocks_ is empty, so no index entries can
    // reference any prior seq; reusing seqs (or not) makes no observable
    // difference. Resetting to 0 would also be fine.
}

void LineBuffer::invalidateWrapCaches()
{
    for (auto &b : blocks_) {
        b.invalidateWrapCache();
    }
    invalidateSumCache();
}

void LineBuffer::enforceLimits()
{
    while ((maxLogicalLines_ > 0 && totalLines_ > maxLogicalLines_) ||
           (maxTotalCells_ > 0 && totalCells_ > maxTotalCells_)) {
        if (blocks_.empty()) {
            break;
        }
        LogicalLineBlock &head = blocks_.front();
        if (head.empty()) {
            blocks_.pop_front();
            ++firstBlockSeq_;
            // Cache deques shadow blocks_: pop the now-stale front entry.
            // No base-offset bump needed: an empty block contributed 0 to
            // both prefix sums when it was pushed, so its stored prefix is
            // equal to the previous block's, and removing it doesn't change
            // any other entry's meaning.
            if (!cachedBlockEndCum_.empty()) {
                cachedBlockEndCum_.pop_front();
            }
            if (!cachedBlockEndLogical_.empty()) {
                cachedBlockEndLogical_.pop_front();
            }
            continue;
        }
        const uint64_t evictedId   = head.lineId(0);
        const int len              = head.lineLength(0);
        // Capture the line's wrapped-row contribution at the current cache
        // width BEFORE dropping it; we need it to bump sumBaseOffset_ so
        // the surviving cache entries continue to resolve to the correct
        // live values.
        int evictedLineWrappedRows = 0;
        if (cachedSumWidth_ != -1) {
            evictedLineWrappedRows = head.numWrappedRowsForLine(0, cachedSumWidth_);
        }
        const bool blockEmpty = head.dropFront(1);
        totalLines_ -= 1;
        totalCells_ -= len;

        // Maintain the prefix-sum caches incrementally in their absolute
        // frame: anything that gets evicted is folded into the base offset
        // so surviving stored values keep their meaning.
        if (cachedSumWidth_ != -1) {
            sumBaseOffset_ += evictedLineWrappedRows;
        }
        if (cachedLogicalValid_) {
            logicalBaseOffset_ += 1;
        }

        if (blockEmpty) {
            blocks_.pop_front();
            // firstBlockSeq_ tracks blocks_.front()'s seq. Bump it so that
            // surviving entries (which still hold their original blockSeq)
            // continue to map to the correct deque index via blockSeq -
            // firstBlockSeq_.
            ++firstBlockSeq_;
            // The popped block's cache entry equals the new base offset
            // exactly (since after the loop above, sumBaseOffset_ has been
            // bumped by the block's full contribution): drop it.
            if (!cachedBlockEndCum_.empty()) {
                cachedBlockEndCum_.pop_front();
            }
            if (!cachedBlockEndLogical_.empty()) {
                cachedBlockEndLogical_.pop_front();
            }
        }
        // Multi-block continuation: a logical line too big for one block is
        // split across consecutive blocks, each with the same lineId as its
        // first line. If the evicted entry was the first part of such a line
        // and a continuation survives, re-point the index instead of erasing
        // (the line still exists in scrollback).
        bool continued = false;
        if (!blocks_.empty()) {
            const auto &nb = blocks_.front();
            if (nb.numLines() > 0 && nb.lineId(0) == evictedId) {
                lineIdIndex_[evictedId] = LineLocation { firstBlockSeq_, nb.firstValidLine() };
                continued               = true;
            }
        }
        if (!continued) {
            lineIdIndex_.erase(evictedId);
            if (onLineIdEvicted_) {
                onLineIdEvicted_(evictedId);
            }
        }
    }
    // Re-derive total live wrapped rows from the (now possibly shrunken)
    // back cache entry minus the (now possibly increased) base offset. O(1).
    if (cachedSumWidth_ != -1) {
        cachedTotalWrappedRows_ =
            cachedBlockEndCum_.empty()
            ? 0
            : (cachedBlockEndCum_.back() - sumBaseOffset_);
    }
}
