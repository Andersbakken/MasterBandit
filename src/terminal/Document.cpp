#include "Document.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <re2/re2.h>

// =========================================================================
// Document
//
// Visible grid: a ring of `screenHeight_` rows × `cols_` cells, allocated
// in fixed-size segments (today: 64 rows / segment). Physical-slot-indexed
// metadata: `ringExtras_`, `rowFlags_`. ringHead_ rotates as rows scroll
// up; the displaced row is appended to `scrollback_`.
//
// Scrollback: a `LineBuffer` of variable-length logical lines with
// display-time wrap and per-(line, width) MRU caching. Resize is a no-op
// on storage; only the wrap caches invalidate.
// =========================================================================

int Document::roundUpPow2(int v)
{
    if (v <= 0) {
        return 1;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

void Document::freeSegments()
{
    for (Cell *seg : segments_) {
        std::free(seg);
    }
    segments_.clear();
}

void Document::allocSegments(int from, int to)
{
    segments_.resize(to);
    for (int s = from; s < to; ++s) {
        segments_[s] = static_cast<Cell *>(
            std::calloc(static_cast<size_t>(SEG_SIZE) * cols_, sizeof(Cell)));
    }
}

int Document::screenRowToPhysical(int screenRow) const
{
    return (ringHead_ - screenHeight_ + screenRow) & ringMask();
}

void Document::clearPhysicalRow(int physical)
{
    Cell *r = rowPtr(physical);
    if (eraseBlankIsTrivial()) {
        // Cell is trivially copyable; memset is well-defined to value-initialize
        // the contiguous bytes and avoids per-cell branch overhead in the
        // scrolling hot path (one full-region scrollUp per LF).
        std::memset(r, 0, static_cast<size_t>(cols_) * sizeof(Cell));
    } else {
        // BCE: fill with current SGR bg.
        for (int c = 0; c < cols_; ++c) {
            r[c] = eraseBlank_;
        }
    }
    if (!ringExtras_[physical].empty()) {
        ringExtras_[physical].clear();
    }
    rowFlags_[physical] = 0;
}

void Document::wireScrollbackEviction()
{
    scrollback_.setOnLineIdEvicted([this](uint64_t id)
                                   {
                                       if (onLineIdEvicted_) {
                                           onLineIdEvicted_(id);
                                       }
                                   });
}

// --- Constructors / destructor / move ---

Document::Document() = default;

Document::Document(int cols, int screenHeight,
                   int maxLogicalLines, int maxTotalCells)
    : cols_(cols)
    , screenHeight_(screenHeight)
    , scrollback_(maxLogicalLines, maxTotalCells)
{
    ringCapacity_ = roundUpPow2(screenHeight_ + SEG_SIZE);
    allocSegments(0, ringCapacity_ >> SEG_SHIFT);
    ringExtras_.resize(ringCapacity_);
    rowFlags_.assign(ringCapacity_, 0);
    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
    ringHead_ = screenHeight_;

    screenLineId_.assign(screenHeight_, 0);
    for (int i = 0; i < screenHeight_; ++i) {
        screenLineId_[i] = nextLineId_++;
    }

    wireScrollbackEviction();
}

Document::~Document()
{
    freeSegments();
}

Document::Document(Document &&o) noexcept
    : cols_(o.cols_)
    , screenHeight_(o.screenHeight_)
    , ringCapacity_(o.ringCapacity_)
    , ringHead_(o.ringHead_)
    , segments_(std::move(o.segments_))
    , ringExtras_(std::move(o.ringExtras_))
    , rowFlags_(std::move(o.rowFlags_))
    , scrollback_(std::move(o.scrollback_))
    , screenLineId_(std::move(o.screenLineId_))
    , nextLineId_(o.nextLineId_)
    , dirty_(std::move(o.dirty_))
    , allDirty_(o.allDirty_)
    , wrapBuffer_(std::move(o.wrapBuffer_))
    , wrapBufferExtras_(std::move(o.wrapBufferExtras_))
    , wrapBufferRowIdx_(o.wrapBufferRowIdx_)
    , wrapBufferWidth_(o.wrapBufferWidth_)
    , wrapBufferContinued_(o.wrapBufferContinued_)
    , onLineIdEvicted_(std::move(o.onLineIdEvicted_))
{
    o.ringCapacity_ = 0;
    o.cols_         = 0;
    wireScrollbackEviction();
}

Document &Document::operator=(Document &&o) noexcept
{
    if (this == &o) {
        return *this;
    }
    freeSegments();
    cols_                = o.cols_;
    o.cols_              = 0;
    screenHeight_        = o.screenHeight_;
    ringCapacity_        = o.ringCapacity_;
    o.ringCapacity_      = 0;
    ringHead_            = o.ringHead_;
    segments_            = std::move(o.segments_);
    ringExtras_          = std::move(o.ringExtras_);
    rowFlags_            = std::move(o.rowFlags_);
    scrollback_          = std::move(o.scrollback_);
    screenLineId_        = std::move(o.screenLineId_);
    nextLineId_          = o.nextLineId_;
    dirty_               = std::move(o.dirty_);
    allDirty_            = o.allDirty_;
    wrapBuffer_          = std::move(o.wrapBuffer_);
    wrapBufferExtras_    = std::move(o.wrapBufferExtras_);
    wrapBufferRowIdx_    = o.wrapBufferRowIdx_;
    wrapBufferWidth_     = o.wrapBufferWidth_;
    wrapBufferContinued_ = o.wrapBufferContinued_;
    onLineIdEvicted_     = std::move(o.onLineIdEvicted_);
    wireScrollbackEviction();
    return *this;
}

void Document::setOnLineIdEvicted(std::function<void(uint64_t)> cb)
{
    onLineIdEvicted_ = std::move(cb);
}

void Document::setMaxLogicalLines(int n)
{
    scrollback_.setMaxLogicalLines(n);
}

void Document::setMaxTotalCells(int n)
{
    scrollback_.setMaxTotalCells(n);
}

// --- Pushing rows from visible to scrollback ---

void Document::pushVisibleRowToScrollback(int screenRow, int physical)
{
    // Determine row's effective length (trim trailing nulls for hard-broken
    // rows; soft-wrapped rows always span all cols_).
    const Cell *r       = rowPtr(physical);
    const uint8_t flags = rowFlags_[physical];
    const bool soft     = (flags & Continued) != 0;
    int len             = cols_;
    if (!soft) {
        // Trim trailing zero cells. The spacer half of a wide character
        // ALSO has wc==0 (with wideSpacer=true) — leaving it in must take
        // priority over trimming, otherwise the rendered scrollback row
        // loses the spacer flag and the renderer falls back to cellSpan=1
        // (i.e. an emoji that was wide on-screen reflows as narrow when
        // scrolled into history). Stride 8 first so the compiler can
        // batch the wc/spacer reads; finish with a 1-stride tail.
        auto trimmable = [](const Cell &c)
        {
            return c.wc == 0 && !c.attrs.wideSpacer();
        };
        while (len >= 8 &&
               trimmable(r[len - 1]) && trimmable(r[len - 2]) &&
               trimmable(r[len - 3]) && trimmable(r[len - 4]) &&
               trimmable(r[len - 5]) && trimmable(r[len - 6]) &&
               trimmable(r[len - 7]) && trimmable(r[len - 8])) {
            len -= 8;
        }
        while (len > 0 && trimmable(r[len - 1])) {
            --len;
        }

        // Kitty graphics, OSC 8 hyperlinks, and inline embedded-terminal
        // anchors stamp a CellExtra without writing wc, so the cells they
        // occupy look "empty" to the trim loop above. After resize, the
        // re-emit step slices extras by [0, rowLen) — anything past the
        // trimmed length silently drops. Extend len out to the rightmost
        // column with an extra so reflow preserves the image / link / anchor.
        const auto &exMap = ringExtras_[physical];
        for (const auto &[col, ex] : exMap) {
            if (col >= len) {
                len = col + 1;
            }
        }
        if (len > cols_) {
            len = cols_;
        }
    }

    LineMeta::Eol eol = soft ? LineMeta::EolSoft : LineMeta::EolHard;

    const uint64_t lineId  = (screenRow >= 0 && screenRow < static_cast<int>(screenLineId_.size()))
         ? screenLineId_[screenRow]
         : ++nextLineId_;
    // If the row's line ID matches scrollback's last partial-line ID, this
    // row continues that line. lastLineId() is O(1) (reads blocks_.back()
    // directly); lineIdAtLogicalIndex goes through the prefix-sum cache and
    // dominated profiles on streaming-at-cap.
    const bool extendsLast = scrollback_.lastLineIsPartial() &&
        scrollback_.lastLineId() == lineId;

    const auto &extras = ringExtras_[physical];
    scrollback_.appendLine(r, len, eol, /*partial*/ soft, extendsLast, lineId, static_cast<uint8_t>(flags & ~Continued), extras.empty() ? nullptr : &extras);
}

// --- IGrid implementation ---

Cell &Document::cell(int col, int screenRow)
{
    return rowPtr(screenRowToPhysical(screenRow))[col];
}

const Cell &Document::cell(int col, int screenRow) const
{
    return rowPtr(screenRowToPhysical(screenRow))[col];
}

Cell *Document::row(int screenRow)
{
    return rowPtr(screenRowToPhysical(screenRow));
}

const Cell *Document::row(int screenRow) const
{
    return rowPtr(screenRowToPhysical(screenRow));
}

void Document::markRowDirty(int screenRow)
{
    if (screenRow >= 0 && screenRow < screenHeight_) {
        dirty_[screenRow] = true;
    }
}

void Document::markAllDirty()
{
    // Hot during scrolling: every full-region scrollUp marks the whole
    // grid dirty. allDirty_ short-circuits isRowDirty/anyDirty, so the
    // per-row fill is redundant when allDirty_ is already set.
    if (allDirty_) {
        return;
    }
    allDirty_ = true;
    std::fill(dirty_.begin(), dirty_.end(), true);
}

void Document::clearDirty(int screenRow)
{
    if (screenRow >= 0 && screenRow < screenHeight_) {
        dirty_[screenRow] = false;
    }
}

void Document::clearAllDirty()
{
    allDirty_ = false;
    std::fill(dirty_.begin(), dirty_.end(), false);
}

bool Document::isRowDirty(int screenRow) const
{
    if (allDirty_) {
        return true;
    }
    if (screenRow >= 0 && screenRow < screenHeight_) {
        return dirty_[screenRow];
    }
    return false;
}

bool Document::anyDirty() const
{
    if (allDirty_) {
        return true;
    }
    for (int r = 0; r < screenHeight_; ++r) {
        if (dirty_[r]) {
            return true;
        }
    }
    return false;
}

void Document::clearRow(int screenRow)
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return;
    }
    int phys = screenRowToPhysical(screenRow);
    Cell *r  = rowPtr(phys);
    for (int c = 0; c < cols_; ++c) {
        r[c] = eraseBlank_;
    }
    ringExtras_[phys].clear();
    rowFlags_[phys] = 0;
    markRowDirty(screenRow);
}

void Document::clearRow(int screenRow, int startCol, int endCol)
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return;
    }
    startCol = std::max(0, startCol);
    endCol   = std::min(cols_, endCol);
    int phys = screenRowToPhysical(screenRow);
    Cell *r  = rowPtr(phys);
    for (int c = startCol; c < endCol; ++c) {
        r[c] = eraseBlank_;
    }
    if (startCol == 0 && endCol == cols_) {
        ringExtras_[phys].clear();
        rowFlags_[phys] = 0;
    } else {
        for (int c = startCol; c < endCol; ++c) {
            ringExtras_[phys].erase(c);
        }
        if (endCol == cols_) {
            setRowFlag(phys, Continued, false);
        }
        if (rowFlag(phys, HasWide)) {
            bool stillHasWide = false;
            for (int c = 0; c < cols_; ++c) {
                if (r[c].attrs.wide() || r[c].attrs.wideSpacer()) {
                    stillHasWide = true;
                    break;
                }
            }
            setRowFlag(phys, HasWide, stillHasWide);
        }
    }
    markRowDirty(screenRow);
}

void Document::scrollUp(int top, int bottom, int n)
{
    if (top < 0 || bottom > screenHeight_ || top >= bottom || n <= 0) {
        return;
    }
    n = std::min(n, bottom - top);

    if (top == 0 && bottom == screenHeight_) {
        // Full-region scroll: top rows scroll into scrollback, ring rotates.
        for (int i = 0; i < n; ++i) {
            int evictPhys = (ringHead_ - screenHeight_) & ringMask();
            pushVisibleRowToScrollback(0, evictPhys);
            // Shift screen line IDs up; new bottom row gets a fresh ID.
            for (int j = 1; j < screenHeight_; ++j) {
                screenLineId_[j - 1] = screenLineId_[j];
            }
            screenLineId_[screenHeight_ - 1] = nextLineId_++;
            // Clear the slot becoming the new bottom row.
            clearPhysicalRow(ringHead_);
            ringHead_ = (ringHead_ + 1) & ringMask();
        }
        // Wrap cache for scrollback unchanged at current width — appended
        // lines extend it. Old wrapBuffer_ may now be stale.
        wrapBufferRowIdx_ = -1;
        markAllDirty();
    } else if (top == 0) {
        // Partial region from top: rows go to scrollback; bottom rows stay.
        int frozenCount = screenHeight_ - bottom;
        std::vector<Cell> frozen(static_cast<size_t>(frozenCount) * cols_);
        std::vector<std::unordered_map<int, CellExtra>> frozenExtras(frozenCount);
        std::vector<uint8_t> frozenFlags(frozenCount, 0);
        std::vector<uint64_t> frozenLineIds(frozenCount, 0);
        for (int i = 0; i < frozenCount; ++i) {
            int phys = screenRowToPhysical(bottom + i);
            std::memcpy(&frozen[static_cast<size_t>(i) * cols_], rowPtr(phys), cols_ * sizeof(Cell));
            frozenExtras[i]  = std::move(ringExtras_[phys]);
            frozenFlags[i]   = rowFlags_[phys];
            frozenLineIds[i] = screenLineId_[bottom + i];
        }

        for (int i = 0; i < n; ++i) {
            int evictPhys = (ringHead_ - screenHeight_) & ringMask();
            pushVisibleRowToScrollback(0, evictPhys);
            for (int j = 1; j < screenHeight_; ++j) {
                screenLineId_[j - 1] = screenLineId_[j];
            }
            screenLineId_[screenHeight_ - 1] = nextLineId_++;
            clearPhysicalRow(ringHead_);
            ringHead_ = (ringHead_ + 1) & ringMask();
        }

        for (int i = 0; i < frozenCount; ++i) {
            int phys = screenRowToPhysical(bottom + i);
            std::memcpy(rowPtr(phys), &frozen[static_cast<size_t>(i) * cols_], cols_ * sizeof(Cell));
            ringExtras_[phys]         = std::move(frozenExtras[i]);
            rowFlags_[phys]           = frozenFlags[i];
            screenLineId_[bottom + i] = frozenLineIds[i];
        }
        for (int r = bottom - n; r < bottom; ++r) {
            clearRow(r);
            screenLineId_[r] = nextLineId_++;
        }
        wrapBufferRowIdx_ = -1;
        markAllDirty();
    } else {
        // Internal region (not from top): in-place shift, no scrollback push.
        for (int r = top; r < bottom - n; ++r) {
            int dstPhys = screenRowToPhysical(r);
            int srcPhys = screenRowToPhysical(r + n);
            std::memcpy(rowPtr(dstPhys), rowPtr(srcPhys), cols_ * sizeof(Cell));
            ringExtras_[dstPhys] = std::move(ringExtras_[srcPhys]);
            rowFlags_[dstPhys]   = rowFlags_[srcPhys];
            screenLineId_[r]     = screenLineId_[r + n];
            markRowDirty(r);
        }
        for (int r = bottom - n; r < bottom; ++r) {
            clearRow(r);
            screenLineId_[r] = nextLineId_++;
        }
    }
}

void Document::scrollDown(int top, int bottom, int n)
{
    if (top < 0 || bottom > screenHeight_ || top >= bottom || n <= 0) {
        return;
    }
    n = std::min(n, bottom - top);

    for (int r = bottom - 1; r >= top + n; --r) {
        int dstPhys = screenRowToPhysical(r);
        int srcPhys = screenRowToPhysical(r - n);
        std::memcpy(rowPtr(dstPhys), rowPtr(srcPhys), cols_ * sizeof(Cell));
        ringExtras_[dstPhys] = std::move(ringExtras_[srcPhys]);
        rowFlags_[dstPhys]   = rowFlags_[srcPhys];
        screenLineId_[r]     = screenLineId_[r - n];
        markRowDirty(r);
    }
    for (int r = top; r < top + n; ++r) {
        clearRow(r);
        screenLineId_[r] = nextLineId_++;
    }
}

void Document::deleteChars(int screenRow, int col, int count)
{
    if (screenRow < 0 || screenRow >= screenHeight_ || col < 0 || col >= cols_) {
        return;
    }
    count         = std::min(count, cols_ - col);
    Cell *r       = row(screenRow);
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        std::memmove(&r[col], &r[col + count], remaining * sizeof(Cell));
    }
    for (int c = cols_ - count; c < cols_; ++c) {
        r[c] = eraseBlank_;
    }
    int phys = screenRowToPhysical(screenRow);
    auto &ex = ringExtras_[phys];
    if (!ex.empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto &[c, e] : ex) {
            if (c >= col + count) {
                shifted[c - count] = std::move(e);
            } else if (c < col) {
                shifted[c] = std::move(e);
            }
        }
        ex = std::move(shifted);
    }
    markRowDirty(screenRow);
}

void Document::insertChars(int screenRow, int col, int count)
{
    if (screenRow < 0 || screenRow >= screenHeight_ || col < 0 || col >= cols_) {
        return;
    }
    count         = std::min(count, cols_ - col);
    Cell *r       = row(screenRow);
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        std::memmove(&r[col + count], &r[col], remaining * sizeof(Cell));
    }
    for (int c = col; c < col + count; ++c) {
        r[c] = eraseBlank_;
    }
    int phys = screenRowToPhysical(screenRow);
    auto &ex = ringExtras_[phys];
    if (!ex.empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto &[c, e] : ex) {
            if (c >= col && c + count < cols_) {
                shifted[c + count] = std::move(e);
            } else if (c < col) {
                shifted[c] = std::move(e);
            }
        }
        ex = std::move(shifted);
    }
    markRowDirty(screenRow);
}

const CellExtra *Document::getExtra(int col, int screenRow) const
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return nullptr;
    }
    int phys      = screenRowToPhysical(screenRow);
    const auto &m = ringExtras_[phys];
    auto it       = m.find(col);
    return (it != m.end()) ? &it->second : nullptr;
}

CellExtra &Document::ensureExtra(int col, int screenRow)
{
    assert(screenRow >= 0 && screenRow < screenHeight_);
    int phys = screenRowToPhysical(screenRow);
    return ringExtras_[phys][col];
}

void Document::clearExtra(int col, int screenRow)
{
    if (screenRow >= 0 && screenRow < screenHeight_) {
        int phys = screenRowToPhysical(screenRow);
        ringExtras_[phys].erase(col);
    }
}

void Document::clearRowExtras(int screenRow)
{
    if (screenRow >= 0 && screenRow < screenHeight_) {
        int phys = screenRowToPhysical(screenRow);
        ringExtras_[phys].clear();
    }
}

bool Document::isRowContinued(int screenRow) const
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return false;
    }
    int phys = screenRowToPhysical(screenRow);
    return rowFlag(phys, Continued);
}

void Document::setRowContinued(int screenRow, bool v)
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return;
    }
    int phys = screenRowToPhysical(screenRow);
    setRowFlag(phys, Continued, v);
}

void Document::markRowHasWide(int screenRow)
{
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return;
    }
    int phys = screenRowToPhysical(screenRow);
    rowFlags_[phys] |= HasWide;
}

// --- Scrollback API ---

int Document::historySize() const
{
    return scrollback_.numWrappedRows(cols_);
}

int Document::scrollbackLogicalLines() const
{
    return scrollback_.totalLogicalLines();
}

void Document::materializeWrappedRow(int idx) const
{
    if (idx == wrapBufferRowIdx_ && wrapBufferWidth_ == cols_) {
        return;
    }
    LineBuffer::WrappedLineRef ref;
    wrapBuffer_.assign(cols_, Cell {});
    wrapBufferExtras_.clear();
    wrapBufferContinued_ = false;
    wrapBufferRowIdx_    = idx;
    wrapBufferWidth_     = cols_;
    if (!scrollback_.wrappedRowAt(idx, cols_, &ref)) {
        return;
    }
    const Cell *p = scrollback_.cellsAt(ref);
    for (int c = 0; c < ref.rowLength && c < cols_; ++c) {
        wrapBuffer_[c] = p[c];
    }
    // Slice extras: only include extras whose column is in this wrapped row.
    const LineMeta &m = scrollback_.block(ref.blockIdx).meta(ref.lineInBlock);
    for (const auto &[col, ex] : m.extras) {
        if (col >= ref.rowOffset && col < ref.rowOffset + ref.rowLength) {
            wrapBufferExtras_[col - ref.rowOffset] = ex;
        }
    }
    // "continued" = not last wrapped row of a hard-broken line.
    wrapBufferContinued_ = !ref.isLastRowOfLine || ref.eol == LineMeta::EolSoft ||
        ref.eol == LineMeta::EolDwc;
}

const Cell *Document::historyRow(int idx) const
{
    if (idx < 0 || idx >= historySize()) {
        return nullptr;
    }
    materializeWrappedRow(idx);
    return wrapBuffer_.data();
}

const std::unordered_map<int, CellExtra> *Document::historyExtras(int idx) const
{
    if (idx < 0 || idx >= historySize()) {
        return nullptr;
    }
    materializeWrappedRow(idx);
    return wrapBufferExtras_.empty() ? nullptr : &wrapBufferExtras_;
}

bool Document::isHistoryRowContinued(int idx) const
{
    if (idx < 0 || idx >= historySize()) {
        return false;
    }
    materializeWrappedRow(idx);
    return wrapBufferContinued_;
}

void Document::clearHistory()
{
    scrollback_.clear();
    wrapBufferRowIdx_ = -1;
}

void Document::liftRowsAndClearGrid(int topRow, int bottomRow)
{
    // Validate. Inverted or out-of-range ranges turn into a plain
    // clear-everything to keep the failure mode safe (the caller asked
    // for the live grid to be wiped one way or another).
    if (topRow < 0 || bottomRow >= screenHeight_ || topRow > bottomRow) {
        for (int r = 0; r < screenHeight_; ++r) {
            clearPhysicalRow(screenRowToPhysical(r));
            screenLineId_[r] = nextLineId_++;
            dirty_[r]        = true;
        }
        allDirty_         = true;
        wrapBufferRowIdx_ = -1;
        return;
    }

    const int preserveCount = bottomRow - topRow + 1;

    // Snapshot the preserved range BEFORE we start mutating ring slots.
    // Working buffers are local — same shape Document already uses in
    // the partial scrollUp path (Document.cpp:411-422).
    std::vector<Cell> savedCells(static_cast<size_t>(preserveCount) * cols_);
    std::vector<std::unordered_map<int, CellExtra>> savedExtras(preserveCount);
    std::vector<uint8_t> savedFlags(preserveCount, 0);
    std::vector<uint64_t> savedLineIds(preserveCount, 0);
    for (int i = 0; i < preserveCount; ++i) {
        const int src  = topRow + i;
        const int phys = screenRowToPhysical(src);
        std::memcpy(&savedCells[static_cast<size_t>(i) * cols_], rowPtr(phys), cols_ * sizeof(Cell));
        savedExtras[i]  = std::move(ringExtras_[phys]); // emptied in source slot
        savedFlags[i]   = rowFlags_[phys];
        savedLineIds[i] = screenLineId_[src];
    }

    // Wipe every live-grid row. `clearPhysicalRow` zeroes cells, drops
    // extras, clears flags. screenLineId_ is rewritten below; no need to
    // touch it here.
    for (int r = 0; r < screenHeight_; ++r) {
        clearPhysicalRow(screenRowToPhysical(r));
    }

    // Restamp the preserved rows at rows [0, preserveCount).
    for (int i = 0; i < preserveCount; ++i) {
        const int phys = screenRowToPhysical(i);
        std::memcpy(rowPtr(phys), &savedCells[static_cast<size_t>(i) * cols_], cols_ * sizeof(Cell));
        ringExtras_[phys] = std::move(savedExtras[i]);
        rowFlags_[phys]   = savedFlags[i];
        screenLineId_[i]  = savedLineIds[i];
    }

    // Rows below the preserved span get fresh logical-line IDs — they're
    // freshly empty, not soft-wrap continuations of anything.
    for (int r = preserveCount; r < screenHeight_; ++r) {
        screenLineId_[r] = nextLineId_++;
    }

    // The wrap-buffer cache is keyed on history rows; we haven't touched
    // history here. But it can still cache the formerly-visible content
    // that the caller is about to erase via clearHistory() (if they ask)
    // — invalidate to be safe.
    wrapBufferRowIdx_ = -1;

    markAllDirty();
}

const Cell *Document::viewportRow(int viewRow, int viewportOffset) const
{
    if (viewportOffset == 0) {
        return row(viewRow);
    }
    int histSize   = historySize();
    int logicalRow = histSize - viewportOffset + viewRow;
    if (logicalRow < histSize) {
        return historyRow(logicalRow);
    } else {
        int screenRow = logicalRow - histSize;
        if (screenRow < 0 || screenRow >= screenHeight_) {
            return nullptr;
        }
        return row(screenRow);
    }
}

const std::unordered_map<int, CellExtra> *Document::viewportExtras(int viewRow, int viewportOffset) const
{
    if (viewportOffset == 0) {
        int phys      = screenRowToPhysical(viewRow);
        const auto &m = ringExtras_[phys];
        return m.empty() ? nullptr : &m;
    }
    int histSize   = historySize();
    int logicalRow = histSize - viewportOffset + viewRow;
    if (logicalRow < histSize) {
        return historyExtras(logicalRow);
    } else {
        int screenRow = logicalRow - histSize;
        if (screenRow < 0 || screenRow >= screenHeight_) {
            return nullptr;
        }
        int phys      = screenRowToPhysical(screenRow);
        const auto &m = ringExtras_[phys];
        return m.empty() ? nullptr : &m;
    }
}

// --- Line ID resolution ---

uint64_t Document::lineIdForAbs(int abs) const
{
    if (abs < 0) {
        return 0;
    }
    int histSize = historySize();
    if (abs < histSize) {
        LineBuffer::WrappedLineRef ref;
        if (!scrollback_.wrappedRowAt(abs, cols_, &ref)) {
            return 0;
        }
        return scrollback_.block(ref.blockIdx).lineId(ref.lineInBlock);
    }
    int screenRow = abs - histSize;
    if (screenRow < 0 || screenRow >= screenHeight_) {
        return 0;
    }
    return screenLineId_[screenRow];
}

int Document::firstAbsOfLine(uint64_t id) const
{
    if (id == 0) {
        return -1;
    }
    // Check scrollback first: a soft-wrap chain that straddles scrollback
    // and visible grid (partial last line in scrollback shares its id with
    // its continuation in the visible grid) has its FIRST physical row in
    // scrollback. O(1) hash lookup + O(64) sum within the target block.
    if (auto loc = scrollback_.findLine(id)) {
        int wrappedRow = scrollback_.numWrappedRowsBeforeBlock(loc->blockIdx, cols_);
        const auto &b  = scrollback_.block(loc->blockIdx);
        for (int li = 0; li < loc->externalLineIdx; ++li) {
            wrappedRow += b.numWrappedRowsForLine(li, cols_);
        }
        return wrappedRow;
    }
    // Fallback: visible grid.
    for (int i = 0; i < screenHeight_; ++i) {
        if (screenLineId_[i] == id) {
            return historySize() + i;
        }
    }
    return -1;
}

int Document::lastAbsOfLine(uint64_t id) const
{
    if (id == 0) {
        return -1;
    }
    // Visible grid: scan back-to-front since chains end at the bottom of
    // their span.
    int lastScreen = -1;
    for (int i = 0; i < screenHeight_; ++i) {
        if (screenLineId_[i] == id) {
            lastScreen = i;
        }
    }
    if (lastScreen >= 0) {
        return historySize() + lastScreen;
    }
    // Scrollback. The index points at the FIRST block holding this line
    // (so firstAbsOfLine works correctly), so the last row is the start of
    // that line's first continuation, plus that continuation's wrap rows,
    // plus any further continuations in subsequent blocks (a logical line
    // too long for one block is split across consecutive blocks, each with
    // the same lineId as its first line).
    if (auto loc = scrollback_.findLine(id)) {
        int wrappedRow = scrollback_.numWrappedRowsBeforeBlock(loc->blockIdx, cols_);
        const auto &b  = scrollback_.block(loc->blockIdx);
        for (int li = 0; li < loc->externalLineIdx; ++li) {
            wrappedRow += b.numWrappedRowsForLine(li, cols_);
        }
        wrappedRow += b.numWrappedRowsForLine(loc->externalLineIdx, cols_);
        int blockIdx = loc->blockIdx + 1;
        while (blockIdx < scrollback_.blockCount()) {
            const auto &nb = scrollback_.block(blockIdx);
            if (nb.numLines() == 0 || nb.lineId(0) != id) {
                break;
            }
            wrappedRow += nb.numWrappedRowsForLine(0, cols_);
            ++blockIdx;
        }
        return wrappedRow - 1;
    }
    return -1;
}

uint64_t Document::newestLineId() const
{
    if (screenLineId_.empty()) {
        return scrollback_.lastLineId();
    }
    return screenLineId_.back();
}

void Document::inheritLineIdFromAboveScreen(int screenRow)
{
    if (screenRow <= 0 || screenRow >= screenHeight_) {
        return;
    }
    screenLineId_[screenRow] = screenLineId_[screenRow - 1];
}

std::string Document::getTextFromLines(uint64_t startLineId, uint64_t endLineId,
                                       int startCol, int endCol) const
{
    int startAbs = firstAbsOfLine(startLineId);
    int endAbs   = lastAbsOfLine(endLineId);
    if (startAbs < 0) {
        return {};
    }
    if (endAbs < 0) {
        endAbs = historySize() + screenHeight_ - 1;
    }
    if (startAbs > endAbs) {
        return {};
    }
    endAbs = std::min(endAbs, historySize() + screenHeight_ - 1);

    std::string out;
    int histSize = historySize();

    for (int abs = startAbs; abs <= endAbs; ++abs) {
        const Cell *r = nullptr;
        int rowLen    = cols_;
        if (abs < histSize) {
            // Use scrollback's actual row length (may be < cols_).
            LineBuffer::WrappedLineRef ref;
            if (scrollback_.wrappedRowAt(abs, cols_, &ref)) {
                r      = scrollback_.cellsAt(ref);
                rowLen = ref.rowLength;
            }
        } else {
            r      = rowPtr(screenRowToPhysical(abs - histSize));
            rowLen = cols_;
        }
        if (!r) {
            continue;
        }

        int colStart = (abs == startAbs) ? std::max(0, startCol) : 0;
        int colEnd   = (abs == endAbs)
              ? std::min(rowLen, endCol == std::numeric_limits<int>::max() ? rowLen : endCol)
              : rowLen;
        while (colEnd > colStart && r[colEnd - 1].wc == 0) {
            colEnd--;
        }
        for (int c = colStart; c < colEnd; ++c) {
            char32_t cp = r[c].wc;
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
        if (abs < endAbs) {
            out += '\n';
        }
    }
    return out;
}

// --- Resize ---

void Document::resetVisibleGrid(int newCols, int newRows)
{
    freeSegments();
    cols_         = newCols;
    screenHeight_ = newRows;
    ringCapacity_ = roundUpPow2(newRows + SEG_SIZE);
    allocSegments(0, ringCapacity_ >> SEG_SHIFT);
    ringExtras_.assign(ringCapacity_, {});
    rowFlags_.assign(ringCapacity_, 0);
    dirty_.assign(newRows, true);
    allDirty_ = true;
    ringHead_ = newRows;
    screenLineId_.assign(newRows, 0);
    for (int i = 0; i < newRows; ++i) {
        screenLineId_[i] = nextLineId_++;
    }
}

void Document::resize(int newCols, int newRows, CursorTrack *cursor)
{
    if (newCols == cols_ && newRows == screenHeight_) {
        return;
    }

    // Initial construction (default-constructor path).
    if (ringCapacity_ == 0) {
        cols_         = newCols;
        screenHeight_ = newRows;
        ringCapacity_ = roundUpPow2(newRows + SEG_SIZE);
        allocSegments(0, ringCapacity_ >> SEG_SHIFT);
        ringExtras_.resize(ringCapacity_);
        rowFlags_.assign(ringCapacity_, 0);
        dirty_.assign(newRows, true);
        allDirty_ = true;
        ringHead_ = newRows;
        screenLineId_.assign(newRows, 0);
        for (int i = 0; i < newRows; ++i) {
            screenLineId_[i] = nextLineId_++;
        }
        wireScrollbackEviction();
        return;
    }

    resizeReflow(newCols, newRows, cursor);

    // Wrap caches for scrollback may be stale at the new width.
    if (newCols != cols_) {
        scrollback_.invalidateWrapCaches();
    }
    wrapBufferRowIdx_ = -1;

    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
}

void Document::resizeReflow(int newCols, int newRows, CursorTrack *cursor)
{
    // Step 1: Compute "used" rows of the visible grid (rows up through the
    // last non-blank, or up through the cursor — whichever is farther down).
    // Trailing blank rows below cursor are pure padding and shouldn't go to
    // scrollback.
    const int oldHistSize = historySize();
    int cursorScreenRow   = -1;
    int cursorAbsCol      = -1;
    if (cursor) {
        cursorScreenRow = cursor->srcY - oldHistSize;
        cursorAbsCol    = cursor->srcX;
    }

    int usedRows = screenHeight_;
    while (usedRows > 0) {
        int phys      = screenRowToPhysical(usedRows - 1);
        const Cell *r = rowPtr(phys);
        bool blank    = true;
        for (int c = 0; c < cols_ && blank; ++c) {
            if (r[c].wc != 0) {
                blank = false;
            }
        }
        if (rowFlag(phys, Continued)) {
            break;
        }
        if (!blank) {
            break;
        }
        if (cursorScreenRow >= 0 && cursorScreenRow == usedRows - 1) {
            break;
        }
        usedRows--;
    }
    // Always include at least cursor's row if cursor is set AND in range.
    if (cursorScreenRow >= 0 && cursorScreenRow < screenHeight_ &&
        usedRows < cursorScreenRow + 1) {
        usedRows = cursorScreenRow + 1;
    }
    // No content + no screen = nothing to push.
    if (screenHeight_ <= 0) {
        usedRows = 0;
    }

    // Step 2: Push used visible rows to scrollback. While doing so, record
    // the cursor's "offset within its logical line" so we can find it again
    // after the pop-back.
    //
    // The cursor's logical line begins at the visible-grid row whose
    // screenLineId_ first equals screenLineId_[cursorScreenRow] (walking
    // backward through Continued rows). The cursor offset is the sum of
    // (lengths of preceding rows in the same chain) + cursor.x.
    uint64_t cursorLineId  = 0;
    int cursorOffsetInLine = -1; // -1 = not tracking
    int cursorChainStart   = -1;
    if (cursorScreenRow >= 0 && cursorScreenRow < screenHeight_) {
        cursorLineId     = screenLineId_[cursorScreenRow];
        cursorChainStart = cursorScreenRow;
        while (cursorChainStart > 0 &&
               screenLineId_[cursorChainStart - 1] == cursorLineId) {
            --cursorChainStart;
        }
        // The chain's first row may be a continuation of a partial scrollback
        // line — in that case the scrollback's last logical line shares the ID,
        // and the cursor offset includes the cells of that scrollback line.
        int prefixCells = 0;
        if (scrollback_.lastLineIsPartial() &&
            scrollback_.lastLineId() == cursorLineId) {
            // The partial line is the last line of the last block — read
            // its length directly to avoid the prefix-sum lookup.
            const int blockIdx = scrollback_.blockCount() - 1;
            const auto &b      = scrollback_.block(blockIdx);
            prefixCells        = b.lineLength(b.numLines() - 1);
        }
        for (int r = cursorChainStart; r < cursorScreenRow; ++r) {
            // Cells of row r within its chain — Continued rows take cols_,
            // hard-broken rows would have ended the chain, so all are cols_.
            prefixCells += cols_;
        }
        cursorOffsetInLine = prefixCells + cursorAbsCol;
    }

    for (int i = 0; i < usedRows; ++i) {
        int phys = screenRowToPhysical(i);
        pushVisibleRowToScrollback(i, phys);
    }

    // Step 3: Build the new visible grid (fresh ring at newCols × newRows).
    //
    // We don't replace scrollback_ — its blocks store at their logical width;
    // the wrap calculator reflects newCols when we query at newCols.
    resetVisibleGrid(newCols, newRows);

    // Step 4: Pop back wrapped rows from scrollback into the visible grid.
    //
    // We pop logical lines (entire) one at a time from the bottom of
    // scrollback into a staging vector of cells, then re-emit them into
    // the visible grid wrapping at newCols.
    //
    // The visible grid fills bottom-up: the most recent rows go to the
    // bottom of the screen. If scrollback is shorter than newRows, the
    // excess top rows of the screen stay blank.
    //
    // We need just enough wrapped rows (at newCols) to fill newRows of
    // screen. Pop logical lines until we have that many rows OR scrollback
    // is empty.
    struct StagedLine
    {
        std::vector<Cell> cells;
        LineMeta::Eol eol;
        bool wasPartial;
        uint64_t lineId;
        uint8_t flags;
        std::unordered_map<int, CellExtra> extras;
    };

    std::vector<StagedLine> staged; // bottom-most first
    int rowsAccumulated = 0;
    while (rowsAccumulated < newRows && scrollback_.totalLogicalLines() > 0) {
        auto popped = scrollback_.popLastLine();
        if (!popped.ok) {
            break;
        }
        const int len  = static_cast<int>(popped.cells.size());
        const int rows = (len <= 0) ? 1 : (len + newCols - 1) / newCols;
        StagedLine s;
        s.cells      = std::move(popped.cells);
        s.eol        = popped.eol;
        s.wasPartial = popped.wasPartial;
        s.lineId     = popped.lineId;
        s.flags      = popped.flags;
        s.extras     = std::move(popped.extras);
        staged.push_back(std::move(s));
        rowsAccumulated += rows;
    }

    // Re-emit popped lines in original order (top-most first → bottom).
    std::reverse(staged.begin(), staged.end());

    // Compute total wrapped rows produced.
    int totalRowsProduced = 0;
    std::vector<int> linePerRowStart; // for each emitted row, its line index in `staged`
    std::vector<int> rowOffsetInLine; // for each emitted row, offset into the staged line
    for (int li = 0; li < static_cast<int>(staged.size()); ++li) {
        int len  = static_cast<int>(staged[li].cells.size());
        int rows = (len <= 0) ? 1 : (len + newCols - 1) / newCols;
        for (int r = 0; r < rows; ++r) {
            linePerRowStart.push_back(li);
            rowOffsetInLine.push_back(r * newCols);
        }
        totalRowsProduced += rows;
    }

    // The visible grid has newRows slots. If content overflows (> newRows),
    // the topmost (oldest) emitted rows push back to scrollback. If less,
    // emit at top, leave bottom rows blank (matches the legacy reflow's
    // top-fill semantic: row 0 of visible grid gets the oldest staged row).
    int rowsToPushBack = std::max(0, totalRowsProduced - newRows);
    int firstScreenRow = 0;

    // Push back the overflow rows AS COMPLETE LINES (re-append to scrollback).
    // Walk the staged-line index of row `rowsToPushBack-1`; lines fully before
    // that are pushed back as-is, the line that straddles is pushed back as
    // its prefix (partial=true) and the suffix becomes the first visible line.
    {
        int pushedRows = 0;
        int li         = 0;
        while (pushedRows < rowsToPushBack && li < static_cast<int>(staged.size())) {
            const StagedLine &s = staged[li];
            int len             = static_cast<int>(s.cells.size());
            int rowsThisLine    = (len <= 0) ? 1 : (len + newCols - 1) / newCols;
            if (pushedRows + rowsThisLine <= rowsToPushBack) {
                // Whole line pushed back.
                LineMeta::Eol eol = s.eol;
                bool partial      = s.wasPartial;
                std::unordered_map<int, CellExtra> *exPtr =
                    s.extras.empty() ? nullptr : const_cast<std::unordered_map<int, CellExtra> *>(&s.extras);
                scrollback_.appendLine(s.cells.data(), len, eol, partial,
                                       /*extendsLast*/ false,
                                       s.lineId,
                                       s.flags,
                                       exPtr);
                pushedRows += rowsThisLine;
                ++li;
            } else {
                // Straddling line: push back the prefix as partial.
                int rowsToCutOff = rowsToPushBack - pushedRows;
                int cutOffset    = rowsToCutOff * newCols;
                if (cutOffset > len) {
                    cutOffset = len;
                }
                std::unordered_map<int, CellExtra> prefixExtras;
                std::unordered_map<int, CellExtra> suffixExtras;
                for (const auto &[col, ex] : s.extras) {
                    if (col < cutOffset) {
                        prefixExtras[col] = ex;
                    } else {
                        suffixExtras[col - cutOffset] = ex;
                    }
                }
                scrollback_.appendLine(s.cells.data(), cutOffset, LineMeta::EolSoft, /*partial*/ true,
                                       /*extendsLast*/ false,
                                       s.lineId,
                                       s.flags,
                                       prefixExtras.empty() ? nullptr : &prefixExtras);
                // Replace staged[li] with the suffix (so the rest of the
                // emit-into-visible-grid loop sees only the visible portion).
                StagedLine remaining;
                remaining.cells.assign(s.cells.begin() + cutOffset, s.cells.end());
                remaining.eol        = s.eol;
                remaining.wasPartial = s.wasPartial;
                remaining.lineId     = s.lineId;
                remaining.flags      = s.flags;
                remaining.extras     = std::move(suffixExtras);
                staged[li]           = std::move(remaining);
                pushedRows           = rowsToPushBack;
                // Don't advance li; the suffix is what we now emit.
                break;
            }
        }
        // Erase pushed-back lines from the front of staged.
        if (li > 0) {
            staged.erase(staged.begin(), staged.begin() + li);
        }
    }

    // Now emit `staged` into the visible grid starting at `firstScreenRow`.
    int dstRow = firstScreenRow;
    for (int li = 0; li < static_cast<int>(staged.size()); ++li) {
        const StagedLine &s = staged[li];
        int len             = static_cast<int>(s.cells.size());
        int rows            = (len <= 0) ? 1 : (len + newCols - 1) / newCols;
        for (int wr = 0; wr < rows && dstRow < newRows; ++wr) {
            int srcStart = wr * newCols;
            int rowLen   = std::min(newCols, len - srcStart);
            if (rowLen < 0) {
                rowLen = 0;
            }
            int phys = screenRowToPhysical(dstRow);
            Cell *r  = rowPtr(phys);
            std::memcpy(r, s.cells.data() + srcStart, rowLen * sizeof(Cell));
            for (int c = rowLen; c < newCols; ++c) {
                r[c] = Cell {};
            }
            // Stamp metadata.
            uint8_t f = (wr < rows - 1) ? Continued : 0;
            if (wr == rows - 1) {
                if (s.eol == LineMeta::EolSoft || s.wasPartial) {
                    f |= Continued;
                }
            }
            if (s.flags & LineMeta::HasWide) {
                f |= HasWide;
            }
            // Verify HasWide more conservatively for the slice.
            // (cheap: only set if there's a wide cell in this slice)
            if (!(f & HasWide)) {
                for (int c = 0; c < rowLen; ++c) {
                    if (r[c].attrs.wide() || r[c].attrs.wideSpacer()) {
                        f |= HasWide;
                        break;
                    }
                }
            }
            rowFlags_[phys] = f;
            // Slice extras into this row.
            std::unordered_map<int, CellExtra> rowEx;
            for (const auto &[col, ex] : s.extras) {
                if (col >= srcStart && col < srcStart + rowLen) {
                    rowEx[col - srcStart] = ex;
                }
            }
            ringExtras_[phys]     = std::move(rowEx);
            screenLineId_[dstRow] = s.lineId;
            ++dstRow;
        }
    }

    // Step 5: Cursor projection.
    //
    // The cursor's logical line either:
    //   (a) is now a complete-or-partial line in scrollback (cursor below or
    //       past the visible grid bottom — we placed the partial line back
    //       in scrollback because it didn't fit), OR
    //   (b) is in the visible grid (placed during emit above).
    //
    // For (b) we find the visible-grid row whose screenLineId_ matches and
    // whose offset range covers cursorOffsetInLine. For (a) we'd technically
    // need to keep a re-pull mechanism — but in practice the cursor line is
    // always the LAST partial in scrollback (by definition of being live),
    // and our emit logic pulls partial-last lines into the visible grid
    // whenever they fit. So if the cursor is somehow not in the visible
    // grid, we clamp it to the bottom-most row.
    if (cursor) {
        cursor->dstX = 0;
        cursor->dstY = historySize(); // default: top of screen
        if (cursorOffsetInLine >= 0 && cursorLineId != 0) {
            int placed     = -1;
            int chainStart = -1;
            for (int r = 0; r < newRows; ++r) {
                if (screenLineId_[r] == cursorLineId) {
                    if (chainStart < 0) {
                        chainStart = r;
                    }
                    placed = r;
                }
            }
            if (chainStart >= 0) {
                // Cursor at the end of a line (offset == rowLen) stays on
                // the last row in wrap-pending position, not on a new row.
                int rowIdx    = (cursorOffsetInLine == 0)
                       ? 0
                       : (cursorOffsetInLine - 1) / newCols;
                int colIdx    = cursorOffsetInLine - rowIdx * newCols;
                int targetRow = chainStart + rowIdx;
                if (targetRow > placed) {
                    targetRow = placed;
                }
                cursor->dstY = historySize() + targetRow;
                cursor->dstX = std::min(colIdx, newCols - 1);
            } else {
                // Cursor's line is in scrollback (didn't fit on screen).
                // Place at last row of screen.
                cursor->dstY = historySize() + newRows - 1;
                cursor->dstX = 0;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// findText
// ----------------------------------------------------------------------------
//
// Searches every logical line independently. A "logical line" is the
// shell-emitted line as a single contiguous cell run, regardless of how
// soft-wrapping splits it across visual rows at the current width. Matches
// span at most one logical line (`startLineId == endLineId`).
//
// Algorithm:
//   1. For each logical line, build a per-line UTF-8 buffer plus a parallel
//      byte→cell-column index. (Cell columns are NOT byte offsets — wide
//      glyphs span 2 cells but encode 1+ bytes; combining marks live in
//      CellExtra and aren't included in the search text. Sticking to the
//      `Cell::wc` codepoint stream matches getTextFromLines' notion of
//      "what's on the line".)
//   2. Run substring or regex search on that buffer.
//   3. For each hit, translate the byte range back through byteToCol to
//      cell columns; emit a Match anchored on the line id.
//
// The first visible-grid logical line is merged with the last scrollback
// logical line when they share an id (soft-wrap straddle), so a match
// crossing that boundary isn't missed.

namespace {

struct LineBuf
{
    uint64_t lineId = 0;
    std::string text;
    // For each byte in `text`, the cell-offset within the logical line that
    // it originated from. Length == text.size(). For multi-byte UTF-8
    // characters every byte gets the same offset. "Cell offset" means the
    // value Decoration::startCellOffset / endCellOffset takes — cumulative
    // across visual-row wraps within one logical line, NOT 0..cols-1 per
    // row. resolveDecoration() does `row = firstAbs + off/w; col = off%w`,
    // so a match crossing a visual-wrap boundary resolves correctly.
    std::vector<int> byteToOffset;
    // Cell offset AFTER the last appended cell — resolves to the exclusive
    // end-column for matches that end at byte == text.size().
    int byteToOffsetEnd = 0;
};

// Append cells [from..to) into `out`, growing both `text` and
// `byteToOffset`. `baseOffset` is the cumulative cell offset within the
// logical line at which `cells[from]` lives — for scrollback lines that's
// always 0 (the block stores the whole logical line contiguously); for
// visible-grid multi-row logical lines it's `(rowIdxWithinLine) * cols_`.
//
// Wide-glyph trailing cells (wc == 0) become a single space, mirroring
// getTextFromLines' visible-text contract.
void appendCellsToLineBuf(LineBuf &out, const Cell *cells, int from, int to, int baseOffset)
{
    for (int c = from; c < to; ++c) {
        int off     = baseOffset + c;
        char32_t cp = cells[c].wc;
        if (cp == 0) {
            out.text += ' ';
            out.byteToOffset.push_back(off);
        } else if (cp < 0x80) {
            out.text += static_cast<char>(cp);
            out.byteToOffset.push_back(off);
        } else {
            char buf[4];
            int n = utf8::encode(cp, buf);
            for (int k = 0; k < n; ++k) {
                out.text += buf[k];
                out.byteToOffset.push_back(off);
            }
        }
    }
    out.byteToOffsetEnd = baseOffset + to;
}

// Trim trailing nulls (cells with wc == 0 at the very end of a line are
// padding from the cell ring; we don't want a bare line of spaces to be
// "matchable" by a needle of spaces). Mirrors LineBuffer::lineText.
int trimmedLen(const Cell *cells, int len)
{
    while (len > 0 && cells[len - 1].wc == 0) {
        --len;
    }
    return len;
}

// ASCII-only lowercase. Sufficient for our case-insensitive flag because
// the search text is built from arbitrary codepoints; folding non-ASCII
// would require ICU. Documented limitation.
char asciiToLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}

// Substring search with optional case-insensitivity. `hay` is the line buffer,
// `needle` is the search term. Returns a vector of (byteOffset, byteLength)
// pairs. byteLength == needle.size() always; kept as a pair for symmetry with
// the regex branch.
std::vector<std::pair<size_t, size_t>>
substringMatches(const std::string &hay, std::string_view needle,
                 bool caseSensitive, bool wholeWord, int remainingLimit)
{
    std::vector<std::pair<size_t, size_t>> out;
    if (needle.empty() || hay.size() < needle.size()) {
        return out;
    }
    // Lowercased lookups when case-insensitive; we don't allocate a folded
    // copy of hay — for each candidate position we compare byte-by-byte.
    // O(n*m) worst case; for terminal scrollback this is fine and avoids
    // the allocation overhead.
    auto eqAt = [&](size_t pos) -> bool
    {
        for (size_t k = 0; k < needle.size(); ++k) {
            char a = hay[pos + k];
            char b = needle[k];
            if (!caseSensitive) {
                a = asciiToLower(a);
                b = asciiToLower(b);
            }
            if (a != b) {
                return false;
            }
        }
        return true;
    };
    auto wordOk = [&](size_t pos) -> bool
    {
        if (!wholeWord) {
            return true;
        }
        bool leftOk  = (pos == 0) || !isWordChar(hay[pos - 1]);
        size_t end   = pos + needle.size();
        bool rightOk = (end >= hay.size()) || !isWordChar(hay[end]);
        return leftOk && rightOk;
    };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (!eqAt(i) || !wordOk(i)) {
            continue;
        }
        out.emplace_back(i, needle.size());
        if (remainingLimit > 0 && static_cast<int>(out.size()) >= remainingLimit) {
            break;
        }
        // Skip past this match so we don't return overlapping hits for
        // self-overlapping needles like "aa" in "aaaa".
        i += needle.size() - 1;
    }
    return out;
}

std::vector<std::pair<size_t, size_t>>
regexMatches(const std::string &hay, const re2::RE2 &re, int remainingLimit)
{
    std::vector<std::pair<size_t, size_t>> out;
    // We use RE2::Match (the lower-level entry point) rather than
    // FindAndConsume because the latter requires the caller's submatch
    // arguments to correspond to capture groups in the pattern. User
    // patterns coming from a Cmd+F box won't have groups; FindAndConsume
    // would silently return false and produce zero matches for valid
    // group-less patterns like "[0-9]+".
    //
    // Match(submatch, 1) requests submatch[0] (the whole match). pos is
    // advanced past the match (or by 1 on a zero-width hit) to drive
    // non-overlapping iteration through the line.
    re2::StringPiece textSP(hay);
    size_t pos          = 0;
    const size_t endpos = hay.size();
    re2::StringPiece sub;
    while (pos <= endpos) {
        if (!re.Match(textSP, pos, endpos, re2::RE2::UNANCHORED, &sub, 1)) {
            break;
        }
        size_t matchPos = static_cast<size_t>(sub.data() - hay.data());
        size_t matchLen = sub.size();
        if (matchLen == 0) {
            // Zero-width match (e.g. /^/, /$/). Advance by one byte to
            // make progress; skip emitting the empty hit.
            pos = matchPos + 1;
            continue;
        }
        out.emplace_back(matchPos, matchLen);
        if (remainingLimit > 0 && static_cast<int>(out.size()) >= remainingLimit) {
            break;
        }
        pos = matchPos + matchLen;
    }
    return out;
}

// Convert byte-range matches in `buf.text` to cell-offset Match records
// anchored on `buf.lineId`. The output startCol/endCol fields hold cell
// offsets within the logical line (suitable as Decoration startCellOffset
// / endCellOffset), NOT visual-row columns. Appends to `out`.
void emitMatches(const LineBuf &buf,
                 const std::vector<std::pair<size_t, size_t>> &byteHits,
                 std::vector<Document::Match> &out)
{
    for (auto [bytePos, byteLen] : byteHits) {
        Document::Match m;
        m.startLineId  = buf.lineId;
        m.endLineId    = buf.lineId;
        m.startCol     = (bytePos < buf.byteToOffset.size())
                ? buf.byteToOffset[bytePos]
                : buf.byteToOffsetEnd;
        size_t endByte = bytePos + byteLen;
        if (endByte == 0) {
            m.endCol = m.startCol;
        } else if (endByte <= buf.byteToOffset.size()) {
            // Exclusive: cell offset AFTER the last matched cell. The cell
            // containing the last matched byte is byteToOffset[endByte-1];
            // exclusive end is that cell + 1.
            m.endCol = buf.byteToOffset[endByte - 1] + 1;
        } else {
            m.endCol = buf.byteToOffsetEnd;
        }
        out.push_back(m);
    }
}

} // namespace

std::vector<Document::Match>
Document::findText(std::string_view needle, const FindOptions &opts) const
{
    std::vector<Match> results;
    if (needle.empty()) {
        return results;
    }

    // Build the regex up front (constant per call). Bail to empty result on
    // syntax errors rather than throwing — the JS layer prefers a clean
    // empty list to a try/catch dance.
    //
    // RE2 is the engine: linear-time guarantee (no catastrophic
    // backtracking), so a malicious or accidentally-pathological pattern
    // can't hang the search. Trade-off vs std::regex: no backreferences
    // (`\1`), no lookahead/lookbehind. Pattern syntax is RE2 dialect,
    // close to PCRE/ECMAScript for everything our `/.../` UI accepts.
    std::optional<re2::RE2> compiled;
    if (opts.regex) {
        re2::RE2::Options reOpts;
        reOpts.set_log_errors(false); // silence stderr on bad user input
        reOpts.set_case_sensitive(opts.caseSensitive);
        compiled.emplace(re2::StringPiece(needle.data(), needle.size()), reOpts);
        if (!compiled->ok()) {
            return results;
        }
    }

    const int hardCap = opts.limit > 0 ? opts.limit : std::numeric_limits<int>::max();
    auto remaining    = [&]() -> int
    {
        return std::max(0, hardCap - static_cast<int>(results.size()));
    };

    auto runOnBuffer = [&](const LineBuf &buf)
    {
        if (buf.text.empty()) {
            return;
        }
        std::vector<std::pair<size_t, size_t>> hits;
        if (compiled) {
            hits = regexMatches(buf.text, *compiled, remaining());
        } else {
            hits = substringMatches(buf.text, needle, opts.caseSensitive, opts.wholeWord, remaining());
        }
        emitMatches(buf, hits, results);
    };

    // -------- Scrollback --------
    //
    // Iterate every logical line in scrollback in order (oldest first).
    // The last logical line may be "partial" — soft-wrap-continued in the
    // visible grid (the user's live edit line). The id of that partial is
    // tracked so the visible-grid pass can skip its continuation:
    // resolveDecoration anchors via firstAbsOfLine(id), which points into
    // scrollback, so a visible-grid match at offset≥L_sb would resolve to
    // the wrong abs row. Better to under-match (rare straddles) than to
    // paint highlights on the wrong cells.
    const int blockCount     = scrollback_.blockCount();
    const bool sbLastPartial = scrollback_.lastLineIsPartial();
    uint64_t sbLastPartialId = 0;
    if (sbLastPartial && blockCount > 0) {
        const auto &lastBlock = scrollback_.block(blockCount - 1);
        const int lastN       = lastBlock.numLines();
        if (lastN > 0) {
            sbLastPartialId = lastBlock.lineId(lastN - 1);
        }
    }

    for (int bi = 0; bi < blockCount; ++bi) {
        if (results.size() >= static_cast<size_t>(hardCap) && hardCap > 0) {
            break;
        }
        const auto &block = scrollback_.block(bi);
        const int n       = block.numLines();
        for (int li = 0; li < n; ++li) {
            const Cell *p = block.lineCells(li);
            int len       = trimmedLen(p, block.lineLength(li));
            uint64_t id   = block.lineId(li);

            LineBuf buf;
            buf.lineId = id;
            appendCellsToLineBuf(buf, p, 0, len, /*baseOffset=*/0);
            runOnBuffer(buf);

            if (results.size() >= static_cast<size_t>(hardCap) && hardCap > 0) {
                break;
            }
        }
    }

    // -------- Visible grid --------
    //
    // Walk screenLineId_ and accumulate consecutive same-id rows into one
    // search buffer. Skip logical lines whose id matches the scrollback
    // partial (see comment above on why straddles can't be anchored
    // correctly via line-id decoration). Cell offsets are cumulative within
    // the logical line: the k-th visual row of a multi-row visible-grid
    // logical line contributes cells at offsets `k * cols_ ..
    // k * cols_ + cols_ - 1`. resolveDecoration's `row = firstAbs + off/w`
    // recovers the abs row.
    if (results.size() < static_cast<size_t>(hardCap) || hardCap == 0) {
        int i = 0;
        while (i < screenHeight_) {
            uint64_t id = screenLineId_[i];
            int j       = i;
            while (j < screenHeight_ && screenLineId_[j] == id) {
                ++j;
            }

            // Skip straddle lines (also skips id == 0, which never has
            // valid scrollback presence anyway — defensive).
            if (id != 0 && sbLastPartial && id == sbLastPartialId) {
                i = j;
                continue;
            }

            LineBuf buf;
            buf.lineId = id;
            for (int sr = i; sr < j; ++sr) {
                int phys      = screenRowToPhysical(sr);
                const Cell *p = rowPtr(phys);
                // Intermediate rows of a soft-wrap chain are full-width by
                // construction (pushVisibleRowToScrollback keeps len=cols_
                // for soft rows). Only the last row of the chain may have
                // trailing nulls trimmed — matches getTextFromLines.
                bool isLast   = (sr == j - 1);
                int rowLen    = isLast ? trimmedLen(p, cols_) : cols_;
                int baseOff   = (sr - i) * cols_;
                appendCellsToLineBuf(buf, p, 0, rowLen, baseOff);
            }
            runOnBuffer(buf);
            if (results.size() >= static_cast<size_t>(hardCap) && hardCap > 0) {
                break;
            }
            i = j;
        }
    }

    return results;
}
