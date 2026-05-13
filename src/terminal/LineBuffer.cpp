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
        if (extendsLast) {
            // The partial-last-line lives in the existing last block (or
            // there is no partial line). If the last block returned false
            // for an extendsLast=true call, that's because:
            //   (a) the block lacks a partial last line — in which case the
            //       caller's invariant is broken; treat as a fresh line.
            //   (b) capacity overflow on extension — we seal the block and
            //       restart a continuation as a NEW partial line in a fresh
            //       block (the line ID stays the same).
            // Path (a) is recovered by calling appendLine again with
            // extendsLast=false. Path (b) is the same code path because we
            // open a new block and append as a new line; the only thing we
            // need to preserve is the line ID and the wrap-context.
            blocks_.emplace_back();
            const bool ok = blocks_.back().appendLine(cells, len, eol, partial, false, lineId, flags, extras);
            (void)ok;
            assert(ok);
            ++totalLines_;
        } else {
            blocks_.emplace_back();
            const bool ok = blocks_.back().appendLine(cells, len, eol, partial, false, lineId, flags, extras);
            (void)ok;
            assert(ok);
            ++totalLines_;
        }
    } else if (!extendsLast) {
        ++totalLines_;
    }

    // Index the line. extendsLast on a successful append doesn't add a new
    // line — same lineId, same internal slot — so the existing index entry
    // remains correct; skip in that case. All other paths (new line in the
    // existing block, sealed-then-restart on overflow, fresh new block)
    // produce a new meta_ entry at internal index meta_.size()-1, which is
    // firstValidLine() + numLines() - 1 for the back block.
    if (!(appended && extendsLast)) {
        const int blockIdx    = static_cast<int>(blocks_.size()) - 1;
        const auto &back      = blocks_.back();
        const int internalIdx = back.firstValidLine() + back.numLines() - 1;
        lineIdIndex_[lineId]  = LineLocation {
            firstBlockSeq_ + static_cast<uint64_t>(blockIdx),
            internalIdx
        };
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

void LineBuffer::ensureSumCache(int width) const
{
    // Three states on entry:
    //  (a) Hot at this width and length matches → return.
    //  (b) Hot at a different width → invalidate, rebuild from scratch.
    //  (c) Width matches but the cache is shorter than blocks_ (new blocks
    //      since last validate) → extend the tail.
    //
    // State (c) is the selection-drag hot path: parse worker appends a few
    // blocks while the user drags, the next render-thread query just walks
    // the new tail entries instead of recomputing every block's wrap count.
    // Before the incremental extend, every cache miss walked the entire
    // deque, which dominated the perf profile during selection drags.
    //
    // Front-side mutations (eviction, popLastLine emptying the back block)
    // never reach state (c): afterBackBlockMutation full-invalidates on
    // eviction, and back-block truncation goes through the same helper.
    // So when sizes differ here, the existing prefix is still valid and
    // new blocks are at the tail.

    const size_t nb = blocks_.size();
    if (cachedSumWidth_ == width && cachedBlockEndCum_.size() == nb) {
#ifndef NDEBUG
        // Debug-only paranoia: verify the cache fully on every hot read.
        // Release builds skip and trust the cache. If incremental
        // maintenance ever drifts, this catches it at the next query.
        int verifyTotal = 0;
        for (size_t i = 0; i < nb; ++i) {
            verifyTotal += blocks_[i].numWrappedRows(width);
            assert(cachedBlockEndCum_[i] == verifyTotal &&
                   "LineBuffer sum cache out of sync (wrapped prefix)");
        }
        assert(cachedTotalWrappedRows_ == verifyTotal &&
               "LineBuffer sum cache out of sync (total wrapped)");
#endif
        return;
    }

    size_t start;
    int total;
    if (cachedSumWidth_ == width && cachedBlockEndCum_.size() < nb) {
        // Incremental extend.
        start = cachedBlockEndCum_.size();
        total = cachedBlockEndCum_.empty() ? 0 : cachedBlockEndCum_.back();
    } else {
        // Full rebuild: width changed or cache was invalidated.
        start = 0;
        total = 0;
    }
    cachedBlockEndCum_.resize(nb);
    for (size_t i = start; i < nb; ++i) {
        total += blocks_[i].numWrappedRows(width);
        cachedBlockEndCum_[i] = total;
    }
    cachedTotalWrappedRows_ = total;
    cachedSumWidth_         = width;
}

void LineBuffer::ensureLogicalCache() const
{
    const size_t nb = blocks_.size();
    if (cachedLogicalValid_ && cachedBlockEndLogical_.size() == nb) {
#ifndef NDEBUG
        int verifyTotal = 0;
        for (size_t i = 0; i < nb; ++i) {
            verifyTotal += blocks_[i].numLines();
            assert(cachedBlockEndLogical_[i] == verifyTotal &&
                   "LineBuffer logical cache out of sync");
        }
#endif
        return;
    }

    size_t start;
    int total;
    if (cachedLogicalValid_ && cachedBlockEndLogical_.size() < nb) {
        // Incremental extend (same hot path as the wrap cache: parse worker
        // adds blocks while logical-index queries are warm).
        start = cachedBlockEndLogical_.size();
        total = cachedBlockEndLogical_.empty() ? 0 : cachedBlockEndLogical_.back();
    } else {
        start = 0;
        total = 0;
    }
    cachedBlockEndLogical_.resize(nb);
    for (size_t i = start; i < nb; ++i) {
        total += blocks_[i].numLines();
        cachedBlockEndLogical_[i] = total;
    }
    cachedLogicalValid_ = true;
}

void LineBuffer::afterBackBlockMutation(int preBlockCount, bool evicted)
{
    // Eviction shifts every prefix sum — fall back to full invalidation
    // for both caches.
    if (evicted) {
        invalidateSumCache();
        return;
    }

    const size_t nb = blocks_.size();

    // Wrap-rows cache: incremental update only if it was valid AND its
    // current length matches preBlockCount (i.e. the front-eviction path
    // wasn't taken elsewhere).
    if (cachedSumWidth_ != -1 &&
        cachedBlockEndCum_.size() == static_cast<size_t>(preBlockCount)) {
        if (nb == static_cast<size_t>(preBlockCount)) {
            // Same block count → back block was mutated in place.
            if (nb == 0) {
                cachedTotalWrappedRows_ = 0;
            } else {
                const int newBackRows     = blocks_.back().numWrappedRows(cachedSumWidth_);
                const int prevCum         = (nb >= 2) ? cachedBlockEndCum_[nb - 2] : 0;
                const int newCum          = prevCum + newBackRows;
                cachedBlockEndCum_.back() = newCum;
                cachedTotalWrappedRows_   = newCum;
            }
        } else if (nb > static_cast<size_t>(preBlockCount)) {
            cachedBlockEndCum_.resize(nb);
            int total = (preBlockCount == 0) ? 0 : cachedBlockEndCum_[preBlockCount - 1];
            for (size_t i = static_cast<size_t>(preBlockCount); i < nb; ++i) {
                total += blocks_[i].numWrappedRows(cachedSumWidth_);
                cachedBlockEndCum_[i] = total;
            }
            cachedTotalWrappedRows_ = total;
        } else {
            // nb < preBlockCount — back blocks truncated (popLastLine
            // emptied the tail).
            cachedBlockEndCum_.resize(nb);
            cachedTotalWrappedRows_ = nb == 0 ? 0 : cachedBlockEndCum_.back();
        }
    } else {
        cachedSumWidth_ = -1;
    }

    // Logical-count cache: same shape, width-independent.
    if (cachedLogicalValid_ &&
        cachedBlockEndLogical_.size() == static_cast<size_t>(preBlockCount)) {
        if (nb == static_cast<size_t>(preBlockCount)) {
            if (nb > 0) {
                const int newBackLines        = blocks_.back().numLines();
                const int prevLog             = (nb >= 2) ? cachedBlockEndLogical_[nb - 2] : 0;
                cachedBlockEndLogical_.back() = prevLog + newBackLines;
            }
        } else if (nb > static_cast<size_t>(preBlockCount)) {
            cachedBlockEndLogical_.resize(nb);
            int total = (preBlockCount == 0) ? 0 : cachedBlockEndLogical_[preBlockCount - 1];
            for (size_t i = static_cast<size_t>(preBlockCount); i < nb; ++i) {
                total += blocks_[i].numLines();
                cachedBlockEndLogical_[i] = total;
            }
        } else {
            cachedBlockEndLogical_.resize(nb);
        }
    } else {
        cachedLogicalValid_ = false;
    }
}

int LineBuffer::numWrappedRows(int width) const
{
    if (width <= 0) {
        return 0;
    }
    ensureSumCache(width);
    return cachedTotalWrappedRows_;
}

bool LineBuffer::wrappedRowAt(int wrappedRow, int width, WrappedLineRef *out) const
{
    if (wrappedRow < 0 || width <= 0) {
        return false;
    }
    ensureSumCache(width);
    if (wrappedRow >= cachedTotalWrappedRows_) {
        return false;
    }

    // Binary search: first block whose cumulative end > wrappedRow.
    auto it           = std::upper_bound(cachedBlockEndCum_.begin(),
                               cachedBlockEndCum_.end(),
                               wrappedRow);
    const int bi      = static_cast<int>(it - cachedBlockEndCum_.begin());
    const int prevCum = (bi == 0) ? 0 : cachedBlockEndCum_[bi - 1];
    int localRem      = wrappedRow - prevCum;

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
    return last >= 0 ? cachedBlockEndCum_[last] : 0;
}

bool LineBuffer::resolveLogicalIndex(int idx, int *blockIdx, int *lineInBlock) const
{
    if (idx < 0 || blocks_.empty()) {
        return false;
    }
    ensureLogicalCache();
    // Binary-search the cumulative logical-line-count prefix array.
    // cachedBlockEndLogical_[i] = number of logical lines in blocks_[0..i].
    // The first block whose cumulative end > idx contains line `idx`.
    auto it = std::upper_bound(cachedBlockEndLogical_.begin(),
                               cachedBlockEndLogical_.end(),
                               idx);
    if (it == cachedBlockEndLogical_.end()) {
        return false; // idx >= total lines
    }
    const int bi      = static_cast<int>(it - cachedBlockEndLogical_.begin());
    const int prevCum = (bi == 0) ? 0 : *(it - 1);
    *blockIdx         = bi;
    *lineInBlock      = idx - prevCum;
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
    for (int c = 0; c < trim; ++c) {
        char32_t cp = p[c].wc;
        if (cp == 0) {
            out += ' ';
            continue;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else {
            char buf[4];
            int n = utf8::encode(cp, buf);
            out.append(buf, n);
        }
    }
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
        for (int c = from; c < to; ++c) {
            char32_t cp = p[c].wc;
            if (cp == 0) {
                out += ' ';
                continue;
            }
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else {
                char buf[4];
                int n = utf8::encode(cp, buf);
                out.append(buf, n);
            }
        }
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
            continue;
        }
        const uint64_t evictedId = head.lineId(0);
        const int len            = head.lineLength(0);
        const bool blockEmpty    = head.dropFront(1);
        totalLines_ -= 1;
        totalCells_ -= len;
        lineIdIndex_.erase(evictedId);
        if (onLineIdEvicted_) {
            onLineIdEvicted_(evictedId);
        }
        if (blockEmpty) {
            blocks_.pop_front();
            // firstBlockSeq_ tracks blocks_.front()'s seq. Bump it so that
            // surviving entries (which still hold their original blockSeq)
            // continue to map to the correct deque index via blockSeq -
            // firstBlockSeq_.
            ++firstBlockSeq_;
        }
    }
}

void LineBuffer::recomputeTotals()
{
    totalLines_ = 0;
    totalCells_ = 0;
    for (const auto &b : blocks_) {
        totalLines_ += b.numLines();
        totalCells_ += b.cellsUsed();
    }
}
