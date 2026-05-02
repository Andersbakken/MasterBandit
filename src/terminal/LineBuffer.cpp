#include "LineBuffer.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

// =========================================================================
// LogicalLineBlock
// =========================================================================

LogicalLineBlock::LogicalLineBlock() {
    cells_.reserve(kCellCapacity);
    cumulativeLengths_.reserve(64);
    meta_.reserve(64);
}

bool LogicalLineBlock::appendLine(const Cell* cells, int len,
                                  LineMeta::Eol eol, bool partial, bool extendsLast,
                                  uint64_t lineId, uint8_t flags,
                                  const std::unordered_map<int, CellExtra>* extras)
{
    if (len < 0) return false;

    // For "extend last partial line", the previous line's metadata is mutated
    // and cells are appended.
    if (extendsLast) {
        if (meta_.empty() || firstValidLine_ >= static_cast<int>(meta_.size())) {
            // Cannot extend nothing — caller must seal block and retry without
            // extendsLast.
            return false;
        }
        const int lastIdx = static_cast<int>(meta_.size()) - 1;
        if (!meta_[lastIdx].isPartial) return false;

        // Capacity check: do we have room for `len` more cells?
        if (static_cast<int>(cells_.size()) + len > kCellCapacity && len > 0) {
            // Allow some overflow but not unbounded.
            if (static_cast<int>(cells_.size()) + len > kCellCapacity * 2) return false;
        }
        const int currentEnd = cumulativeLengths_[lastIdx];
        const int prevStart  = (lastIdx == firstValidLine_) ? bufferStartOffset_
                                                            : cumulativeLengths_[lastIdx - 1];
        const int currentLen = currentEnd - prevStart;

        if (len > 0) {
            cells_.insert(cells_.end(), cells, cells + len);
        }
        cumulativeLengths_[lastIdx] = static_cast<int>(cells_.size());

        meta_[lastIdx].eol = eol;
        meta_[lastIdx].isPartial = partial;
        meta_[lastIdx].flags |= flags;
        if (extras) {
            for (const auto& [col, ex] : *extras) {
                if (col < 0 || col >= len) continue;
                meta_[lastIdx].extras[col + currentLen] = ex;
            }
        }
        meta_[lastIdx].cachedWidth = -1;
        cachedWidth_ = -1;
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
    m.flags = flags;
    m.eol = eol;
    m.isPartial = partial;
    m.lineId = lineId;
    if (extras) {
        for (const auto& [col, ex] : *extras) {
            if (col < 0 || col >= len) continue;
            m.extras[col] = ex;
        }
    }
    meta_.push_back(std::move(m));
    cachedWidth_ = -1;
    return true;
}

const Cell* LogicalLineBlock::lineCells(int i) const
{
    const int abs = firstValidLine_ + i;
    const int start = lineStartOffset(abs);
    return cells_.data() + start;
}

int LogicalLineBlock::lineLength(int i) const
{
    const int abs = firstValidLine_ + i;
    const int start = lineStartOffset(abs);
    const int end = cumulativeLengths_[abs];
    return end - start;
}

LineMeta& LogicalLineBlock::mutableMeta(int i)
{
    cachedWidth_ = -1;
    LineMeta& m = meta_[firstValidLine_ + i];
    m.cachedWidth = -1;
    return m;
}

bool LogicalLineBlock::lastIsPartial() const
{
    if (empty()) return false;
    return meta_.back().isPartial;
}

// Number of wrapped rows for one line of `len` cells at `width`.
// HasWide is currently a hint; the wrap calculation is plain integer for
// non-wide content and identical (no DWC special-case yet) for wide content
// because Cells already encode wide+spacer pairs as two cells. A line of N
// cells at width W produces ⌈N / W⌉ rows, with at least 1.
static int wrappedRowsFor(int len, int width)
{
    if (width <= 0) return 1;
    if (len <= 0) return 1;
    return (len + width - 1) / width;
}

int LogicalLineBlock::numWrappedRowsForLine(int i, int width) const
{
    const LineMeta& m = meta(i);
    if (m.cachedWidth == width) return m.cachedWrappedRows;
    const int len = lineLength(i);
    int rows = wrappedRowsFor(len, width);
    m.cachedWidth = width;
    m.cachedWrappedRows = rows;
    return rows;
}

int LogicalLineBlock::numWrappedRows(int width) const
{
    if (cachedWidth_ == width) return cachedWrappedRows_;
    int total = 0;
    for (int i = 0; i < numLines(); ++i) {
        total += numWrappedRowsForLine(i, width);
    }
    cachedWidth_ = width;
    cachedWrappedRows_ = total;
    return total;
}

void LogicalLineBlock::invalidateWrapCache()
{
    cachedWidth_ = -1;
    for (auto& m : meta_) m.cachedWidth = -1;
}

bool LogicalLineBlock::dropFront(int n)
{
    if (n <= 0) return empty();
    const int avail = numLines();
    n = std::min(n, avail);
    firstValidLine_ += n;
    if (firstValidLine_ < static_cast<int>(meta_.size())) {
        bufferStartOffset_ = cumulativeLengths_[firstValidLine_ - 1];
    } else {
        // Block is now empty.
        cells_.clear();
        cumulativeLengths_.clear();
        meta_.clear();
        firstValidLine_ = 0;
        bufferStartOffset_ = 0;
    }
    cachedWidth_ = -1;
    return empty();
}

void LogicalLineBlock::dropLast()
{
    if (empty()) return;
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
{}

LineBuffer::LineBuffer(int maxLogicalLines, int maxTotalCells)
    : maxLogicalLines_(maxLogicalLines)
    , maxTotalCells_(maxTotalCells)
{}

void LineBuffer::setMaxLogicalLines(int n)
{
    maxLogicalLines_ = n;
    enforceLimits();
}

void LineBuffer::setMaxTotalCells(int n)
{
    maxTotalCells_ = n;
    enforceLimits();
}

void LineBuffer::appendLine(const Cell* cells, int len,
                            LineMeta::Eol eol, bool partial, bool extendsLast,
                            uint64_t lineId, uint8_t flags,
                            const std::unordered_map<int, CellExtra>* extras)
{
    // Try to append to the last block. If it fails (capacity, or the
    // extendsLast precondition isn't met because the last line is in a
    // different block), open a new block.
    bool appended = false;
    if (!blocks_.empty()) {
        appended = blocks_.back().appendLine(cells, len, eol, partial, extendsLast,
                                             lineId, flags, extras);
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
            const bool ok = blocks_.back().appendLine(cells, len, eol, partial, false,
                                                     lineId, flags, extras);
            (void)ok;
            assert(ok);
            ++totalLines_;
        } else {
            blocks_.emplace_back();
            const bool ok = blocks_.back().appendLine(cells, len, eol, partial, false,
                                                     lineId, flags, extras);
            (void)ok;
            assert(ok);
            ++totalLines_;
        }
    } else if (!extendsLast) {
        ++totalLines_;
    }

    totalCells_ += len;
    enforceLimits();
}

void LineBuffer::appendHardLine(const Cell* cells, int len,
                                uint64_t lineId, uint8_t flags,
                                const std::unordered_map<int, CellExtra>* extras)
{
    appendLine(cells, len, LineMeta::EolHard, /*partial*/false, /*extendsLast*/false,
               lineId, flags, extras);
}

LineBuffer::PoppedLine LineBuffer::popLastLine()
{
    PoppedLine result;
    if (blocks_.empty()) return result;
    LogicalLineBlock& last = blocks_.back();
    if (last.empty()) {
        blocks_.pop_back();
        return popLastLine();
    }
    const int idx = last.numLines() - 1;
    const Cell* p = last.lineCells(idx);
    const int len = last.lineLength(idx);
    result.cells.assign(p, p + len);
    const LineMeta& m = last.meta(idx);
    result.eol = m.eol;
    result.wasPartial = m.isPartial;
    result.lineId = m.lineId;
    result.flags = m.flags;
    result.extras = m.extras;
    result.ok = true;

    last.dropLast();
    totalCells_ -= len;
    --totalLines_;
    if (last.empty()) blocks_.pop_back();
    return result;
}

int LineBuffer::numWrappedRows(int width) const
{
    int total = 0;
    for (const auto& b : blocks_) total += b.numWrappedRows(width);
    return total;
}

bool LineBuffer::wrappedRowAt(int wrappedRow, int width, WrappedLineRef* out) const
{
    if (wrappedRow < 0 || width <= 0) return false;
    int remaining = wrappedRow;
    for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi) {
        const auto& b = blocks_[bi];
        const int n = b.numWrappedRows(width);
        if (remaining < n) {
            // Find which line within this block.
            int localRem = remaining;
            for (int li = 0; li < b.numLines(); ++li) {
                const int rows = b.numWrappedRowsForLine(li, width);
                if (localRem < rows) {
                    const int len = b.lineLength(li);
                    const int rowOffset = localRem * width;
                    int rowLen = std::min(width, len - rowOffset);
                    if (rowLen < 0) rowLen = 0;
                    out->blockIdx = bi;
                    out->lineInBlock = li;
                    out->rowOffset = rowOffset;
                    out->rowLength = rowLen;
                    out->isFirstRowOfLine = (localRem == 0);
                    out->isLastRowOfLine  = (localRem == rows - 1);
                    out->eol = out->isLastRowOfLine ? b.meta(li).eol : LineMeta::EolSoft;
                    return true;
                }
                localRem -= rows;
            }
        }
        remaining -= n;
    }
    return false;
}

const Cell* LineBuffer::cellsAt(const WrappedLineRef& ref) const
{
    return blocks_[ref.blockIdx].lineCells(ref.lineInBlock) + ref.rowOffset;
}

const Cell* LineBuffer::wrappedRowCells(int wrappedRow, int width, int* outLen) const
{
    WrappedLineRef ref;
    if (!wrappedRowAt(wrappedRow, width, &ref)) return nullptr;
    if (outLen) *outLen = ref.rowLength;
    return cellsAt(ref);
}

bool LineBuffer::lastLineIsPartial() const
{
    if (blocks_.empty()) return false;
    return blocks_.back().lastIsPartial();
}

uint64_t LineBuffer::lineIdAtLogicalIndex(int idx) const
{
    int blockIdx = 0, lineInBlock = 0;
    if (!resolveLogicalIndex(idx, &blockIdx, &lineInBlock)) return 0;
    return blocks_[blockIdx].lineId(lineInBlock);
}

int LineBuffer::logicalIndexOfLineId(uint64_t id) const
{
    if (id == 0) return -1;
    // Lines are appended in monotonic-id order, so we can binary search by
    // first ID in each block. But blocks may overlap in id space if... no,
    // they don't — we never append out of order. Linear over blocks is fine.
    int base = 0;
    for (const auto& b : blocks_) {
        if (b.empty()) continue;
        const uint64_t firstId = b.lineId(0);
        const uint64_t lastId  = b.lineId(b.numLines() - 1);
        if (id < firstId) return -1;  // evicted
        if (id <= lastId) {
            // Linear inside block; numLines is small (~10s).
            for (int i = 0; i < b.numLines(); ++i) {
                if (b.lineId(i) == id) return base + i;
                if (b.lineId(i) > id) return -1;
            }
            return -1;
        }
        base += b.numLines();
    }
    return -1;
}

bool LineBuffer::resolveLogicalIndex(int idx, int* blockIdx, int* lineInBlock) const
{
    if (idx < 0) return false;
    int rem = idx;
    for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi) {
        const int n = blocks_[bi].numLines();
        if (rem < n) {
            *blockIdx = bi;
            *lineInBlock = rem;
            return true;
        }
        rem -= n;
    }
    return false;
}

std::string LineBuffer::lineText(int idx) const
{
    int bi, li;
    if (!resolveLogicalIndex(idx, &bi, &li)) return {};
    const Cell* p = blocks_[bi].lineCells(li);
    const int len = blocks_[bi].lineLength(li);
    std::string out;
    out.reserve(len);
    int trim = len;
    while (trim > 0 && p[trim - 1].wc == 0) --trim;
    for (int c = 0; c < trim; ++c) {
        char32_t cp = p[c].wc;
        if (cp == 0) { out += ' '; continue; }
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
    if (startIdx < 0) startIdx = 0;
    if (endIdx >= totalLines_) endIdx = totalLines_ - 1;
    if (startIdx > endIdx) return {};
    if (endCol < 0) endCol = std::numeric_limits<int>::max();
    std::string out;
    for (int idx = startIdx; idx <= endIdx; ++idx) {
        int bi, li;
        if (!resolveLogicalIndex(idx, &bi, &li)) continue;
        const Cell* p = blocks_[bi].lineCells(li);
        const int len = blocks_[bi].lineLength(li);
        const int from = (idx == startIdx) ? std::max(0, startCol) : 0;
        int to = (idx == endIdx) ? std::min(len, endCol) : len;
        while (to > from && p[to - 1].wc == 0) --to;
        for (int c = from; c < to; ++c) {
            char32_t cp = p[c].wc;
            if (cp == 0) { out += ' '; continue; }
            if (cp < 0x80) out += static_cast<char>(cp);
            else {
                char buf[4];
                int n = utf8::encode(cp, buf);
                out.append(buf, n);
            }
        }
        if (idx < endIdx) out += '\n';
    }
    return out;
}

void LineBuffer::clear()
{
    blocks_.clear();
    totalLines_ = 0;
    totalCells_ = 0;
}

void LineBuffer::invalidateWrapCaches()
{
    for (auto& b : blocks_) b.invalidateWrapCache();
}

void LineBuffer::enforceLimits()
{
    while ((maxLogicalLines_ > 0 && totalLines_ > maxLogicalLines_) ||
           (maxTotalCells_   > 0 && totalCells_ > maxTotalCells_))
    {
        if (blocks_.empty()) break;
        LogicalLineBlock& head = blocks_.front();
        if (head.empty()) {
            blocks_.pop_front();
            continue;
        }
        const uint64_t evictedId = head.lineId(0);
        const int len = head.lineLength(0);
        const bool blockEmpty = head.dropFront(1);
        totalLines_ -= 1;
        totalCells_ -= len;
        if (onLineIdEvicted_) onLineIdEvicted_(evictedId);
        if (blockEmpty) blocks_.pop_front();
    }
}

void LineBuffer::recomputeTotals()
{
    totalLines_ = 0;
    totalCells_ = 0;
    for (const auto& b : blocks_) {
        totalLines_ += b.numLines();
        totalCells_ += b.cellsUsed();
    }
}
