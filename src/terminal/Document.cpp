#include "Document.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <cstring>

// --- SGR helpers ---

static void emitSGR(std::string& out, const CellAttrs& oldA, const CellAttrs& newA) {
    bool needReset = (oldA.bold() && !newA.bold()) || (oldA.italic() && !newA.italic()) ||
                     (oldA.underline() && !newA.underline()) || (oldA.strikethrough() && !newA.strikethrough()) ||
                     (oldA.blink() && !newA.blink()) || (oldA.inverse() && !newA.inverse()) ||
                     (oldA.dim() && !newA.dim()) || (oldA.invisible() && !newA.invisible()) ||
                     (oldA.wide() && !newA.wide()) || (oldA.wideSpacer() && !newA.wideSpacer());

    std::string params;
    auto ap = [&](const char* p) { if (!params.empty()) params += ';'; params += p; };
    auto ai = [&](int v) { if (!params.empty()) params += ';'; params += std::to_string(v); };

    CellAttrs base{};
    const CellAttrs& cmp = needReset ? base : oldA;
    if (needReset) ap("0");

    if (newA.bold() && !cmp.bold()) ap("1");
    if (newA.dim() && !cmp.dim()) ap("2");
    if (newA.italic() && !cmp.italic()) ap("3");
    if (newA.underline() && !cmp.underline()) ap("4");
    if (newA.blink() && !cmp.blink()) ap("5");
    if (newA.inverse() && !cmp.inverse()) ap("7");
    if (newA.invisible() && !cmp.invisible()) ap("8");
    if (newA.strikethrough() && !cmp.strikethrough()) ap("9");

    if (newA.fgMode() != cmp.fgMode() ||
        (newA.fgMode() != CellAttrs::Default &&
         (newA.fgR() != cmp.fgR() || newA.fgG() != cmp.fgG() || newA.fgB() != cmp.fgB()))) {
        if (newA.fgMode() == CellAttrs::Default) ap("39");
        else { ap("38"); ap("2"); ai(newA.fgR()); ai(newA.fgG()); ai(newA.fgB()); }
    }
    if (newA.bgMode() != cmp.bgMode() ||
        (newA.bgMode() != CellAttrs::Default &&
         (newA.bgR() != cmp.bgR() || newA.bgG() != cmp.bgG() || newA.bgB() != cmp.bgB()))) {
        if (newA.bgMode() == CellAttrs::Default) ap("49");
        else { ap("48"); ap("2"); ai(newA.bgR()); ai(newA.bgG()); ai(newA.bgB()); }
    }
    if (newA.wide() != cmp.wide() && newA.wide()) ap("100");
    if (newA.wideSpacer() != cmp.wideSpacer() && newA.wideSpacer()) ap("101");

    if (!params.empty()) { out += '\x1b'; out += '['; out += params; out += 'm'; }
}

static void parseSGRParams(const char* start, int len, CellAttrs& attrs) {
    std::vector<int> params;
    int val = 0; bool hasVal = false;
    for (int i = 0; i < len; ++i) {
        if (start[i] >= '0' && start[i] <= '9') { val = val * 10 + (start[i] - '0'); hasVal = true; }
        else if (start[i] == ';') { params.push_back(hasVal ? val : 0); val = 0; hasVal = false; }
    }
    if (hasVal) params.push_back(val);

    for (size_t i = 0; i < params.size(); ++i) {
        switch (params[i]) {
        case 0: attrs.reset(); break;
        case 1: attrs.setBold(true); break;
        case 2: attrs.setDim(true); break;
        case 3: attrs.setItalic(true); break;
        case 4: attrs.setUnderline(true); break;
        case 5: attrs.setBlink(true); break;
        case 7: attrs.setInverse(true); break;
        case 8: attrs.setInvisible(true); break;
        case 9: attrs.setStrikethrough(true); break;
        case 39: attrs.setFgMode(CellAttrs::Default); break;
        case 49: attrs.setBgMode(CellAttrs::Default); break;
        case 38:
            if (i + 4 < params.size() && params[i+1] == 2) {
                attrs.setFgMode(CellAttrs::RGB);
                attrs.setFg(params[i+2], params[i+3], params[i+4]);
                i += 4;
            }
            break;
        case 48:
            if (i + 4 < params.size() && params[i+1] == 2) {
                attrs.setBgMode(CellAttrs::RGB);
                attrs.setBg(params[i+2], params[i+3], params[i+4]);
                i += 4;
            }
            break;
        case 100: attrs.setWide(true); break;
        case 101: attrs.setWideSpacer(true); break;
        }
    }
}

// =========================================================================
// Document implementation
// =========================================================================

int Document::roundUpPow2(int v) {
    if (v <= 0) return 1;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// --- Segment helpers ---

void Document::freeSegments() {
    for (Cell* seg : segments_) ::operator delete(seg);
    segments_.clear();
}

void Document::allocSegments(int from, int to) {
    // Allocate segments [from, to) without constructing cells.
    segments_.resize(to);
    for (int s = from; s < to; ++s) {
        segments_[s] = static_cast<Cell*>(
            ::operator new(static_cast<size_t>(SEG_SIZE) * cols_ * sizeof(Cell)));
    }
}

// --- Constructors / destructor / move ---

Document::Document() = default;

Document::Document(int cols, int screenHeight, int tier1Capacity, int maxArchiveRows)
    : cols_(cols)
    , screenHeight_(screenHeight)
    , maxArchiveRows_(maxArchiveRows)
    , tier1Capacity_(tier1Capacity)
{
    // Start at max capacity so growRing() is never called during normal use.
    // Cap to avoid overflow when tier1Capacity is std::numeric_limits<int>::max() (infinite scrollback).
    int initialHistory = (tier1Capacity < std::numeric_limits<int>::max() / 2) ? tier1Capacity : 1024;
    ringCapacity_ = roundUpPow2(initialHistory + screenHeight + SEG_SIZE);
    allocSegments(0, ringCapacity_ >> SEG_SHIFT);
    // Zero-init all segments. History slots will be overwritten by clearPhysicalRow
    // before use, but zeroing upfront is simpler than tracking the boundary.
    for (Cell* seg : segments_) {
        std::memset(seg, 0, static_cast<size_t>(SEG_SIZE) * cols_ * sizeof(Cell));
    }
    ringExtras_.resize(ringCapacity_);
    continued_.assign(ringCapacity_, false);
    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
    ringHead_ = screenHeight_; // screen rows are [0..screenHeight_-1], head is past them
    historyCount_ = 0;
}

Document::~Document() {
    freeSegments();
}

Document::Document(Document&& o) noexcept
    : cols_(o.cols_)
    , screenHeight_(o.screenHeight_)
    , ringCapacity_(o.ringCapacity_)
    , ringHead_(o.ringHead_)
    , historyCount_(o.historyCount_)
    , segments_(std::move(o.segments_))
    , ringExtras_(std::move(o.ringExtras_))
    , continued_(std::move(o.continued_))
    , rowIdBase_(o.rowIdBase_)
    , dirty_(std::move(o.dirty_))
    , allDirty_(o.allDirty_)
    , archive_(std::move(o.archive_))
    , maxArchiveRows_(o.maxArchiveRows_)
    , tier1Capacity_(o.tier1Capacity_)
    , parseBuffer_(std::move(o.parseBuffer_))
{
    o.ringCapacity_ = 0;
    o.cols_ = 0;
}

Document& Document::operator=(Document&& o) noexcept {
    if (this == &o) return *this;
    freeSegments();
    cols_         = o.cols_;         o.cols_ = 0;
    screenHeight_ = o.screenHeight_;
    ringCapacity_ = o.ringCapacity_; o.ringCapacity_ = 0;
    ringHead_     = o.ringHead_;
    historyCount_ = o.historyCount_;
    segments_     = std::move(o.segments_);
    ringExtras_   = std::move(o.ringExtras_);
    continued_    = std::move(o.continued_);
    rowIdBase_    = o.rowIdBase_;
    dirty_        = std::move(o.dirty_);
    allDirty_     = o.allDirty_;
    archive_      = std::move(o.archive_);
    maxArchiveRows_ = o.maxArchiveRows_;
    tier1Capacity_  = o.tier1Capacity_;
    parseBuffer_    = std::move(o.parseBuffer_);
    return *this;
}

// --- Ring helpers ---

int Document::screenRowToPhysical(int screenRow) const {
    return (ringHead_ - screenHeight_ + screenRow) & ringMask();
}

int Document::historyTier1ToPhysical(int tier1Idx) const {
    return (ringHead_ - screenHeight_ - historyCount_ + tier1Idx) & ringMask();
}

void Document::clearPhysicalRow(int physical) {
    Cell* r = rowPtr(physical);
    for (int c = 0; c < cols_; ++c) r[c] = Cell{};
    ringExtras_[physical].clear();
    continued_[physical] = false;
}

void Document::evictToArchive() {
    if (historyCount_ <= 0) return;
    int oldest = historyTier1ToPhysical(0);
    archive_.push_back({serializeRow(rowPtr(oldest), cols_), continued_[oldest]});
    ringExtras_[oldest].clear();
    continued_[oldest] = false;
    if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
        archive_.pop_front();
        ++rowIdBase_;
    }
    historyCount_--;
}

void Document::growRing()
{
    // Hot path: called from scrollUp when the ring is full.
    // Invariant: ringHead_ == 0 when called (data is contiguous at [0, total)).
    // We simply append new segments — no data copying needed.
    int total = historyCount_ + screenHeight_;
    int newCap = roundUpPow2(total + SEG_SIZE);
    if (tier1Capacity_ < std::numeric_limits<int>::max() / 2) {
        int maxCap = roundUpPow2(tier1Capacity_ + screenHeight_ + SEG_SIZE);
        newCap = std::min(newCap, maxCap);
    }
    if (newCap <= ringCapacity_) return;

    int oldNumSegs = ringCapacity_ >> SEG_SHIFT;
    int newNumSegs = newCap >> SEG_SHIFT;
    allocSegments(oldNumSegs, newNumSegs);
    ringExtras_.resize(newCap);
    continued_.resize(newCap, false);
    ringCapacity_ = newCap;
    ringHead_ = total; // first free slot (data is at [0, total))
}

void Document::growRingGeneral(int newCap)
{
    // Slow path: used by resize() where ringHead_ may be non-zero.
    // Linearizes the circular ring into new segments at positions [0, total).
    int total = historyCount_ + screenHeight_;
    int newNumSegs = newCap >> SEG_SHIFT;

    std::vector<Cell*> newSegs(newNumSegs, nullptr);
    for (int s = 0; s < newNumSegs; ++s) {
        newSegs[s] = static_cast<Cell*>(
            ::operator new(static_cast<size_t>(SEG_SIZE) * cols_ * sizeof(Cell)));
    }

    std::vector<std::unordered_map<int, CellExtra>> newExtras(newCap);
    std::vector<bool> newCont(newCap, false);

    for (int i = 0; i < total; ++i) {
        int oldPhys = (ringHead_ - total + i) & ringMask();
        Cell* dst = newSegs[i >> SEG_SHIFT] + (i & SEG_MASK) * cols_;
        std::memcpy(dst, rowPtr(oldPhys), cols_ * sizeof(Cell));
        newExtras[i] = std::move(ringExtras_[oldPhys]);
        newCont[i] = continued_[oldPhys];
    }

    freeSegments();
    segments_ = std::move(newSegs);
    ringExtras_ = std::move(newExtras);
    continued_ = std::move(newCont);
    ringCapacity_ = newCap;
    ringHead_ = total;
}

// --- IGrid implementation ---

Cell& Document::cell(int col, int screenRow) {
    return rowPtr(screenRowToPhysical(screenRow))[col];
}

const Cell& Document::cell(int col, int screenRow) const {
    return rowPtr(screenRowToPhysical(screenRow))[col];
}

Cell* Document::row(int screenRow) {
    return rowPtr(screenRowToPhysical(screenRow));
}

const Cell* Document::row(int screenRow) const {
    return rowPtr(screenRowToPhysical(screenRow));
}

void Document::markRowDirty(int screenRow) {
    if (screenRow >= 0 && screenRow < screenHeight_) dirty_[screenRow] = true;
}

void Document::markAllDirty() {
    allDirty_ = true;
    std::fill(dirty_.begin(), dirty_.end(), true);
}

void Document::clearDirty(int screenRow) {
    if (screenRow >= 0 && screenRow < screenHeight_) dirty_[screenRow] = false;
}

void Document::clearAllDirty() {
    allDirty_ = false;
    std::fill(dirty_.begin(), dirty_.end(), false);
}

bool Document::isRowDirty(int screenRow) const {
    if (allDirty_) return true;
    if (screenRow >= 0 && screenRow < screenHeight_) return dirty_[screenRow];
    return false;
}

bool Document::anyDirty() const {
    if (allDirty_) return true;
    for (int r = 0; r < screenHeight_; ++r) {
        if (dirty_[r]) return true;
    }
    return false;
}

void Document::clearRow(int screenRow) {
    if (screenRow < 0 || screenRow >= screenHeight_) return;
    int phys = screenRowToPhysical(screenRow);
    Cell* r = rowPtr(phys);
    for (int c = 0; c < cols_; ++c) r[c] = Cell{};
    ringExtras_[phys].clear();
    markRowDirty(screenRow);
}

void Document::clearRow(int screenRow, int startCol, int endCol) {
    if (screenRow < 0 || screenRow >= screenHeight_) return;
    startCol = std::max(0, startCol);
    endCol = std::min(cols_, endCol);
    int phys = screenRowToPhysical(screenRow);
    Cell* r = rowPtr(phys);
    for (int c = startCol; c < endCol; ++c) r[c] = Cell{};
    if (startCol == 0 && endCol == cols_) {
        ringExtras_[phys].clear();
    } else {
        for (int c = startCol; c < endCol; ++c) ringExtras_[phys].erase(c);
    }
    markRowDirty(screenRow);
}

void Document::scrollUp(int top, int bottom, int n) {
    if (top < 0 || bottom > screenHeight_ || top >= bottom || n <= 0) return;
    n = std::min(n, bottom - top);

    if (top == 0 && bottom == screenHeight_) {
        // Fast path: advance ring head. Old top rows become history.
        for (int i = 0; i < n; ++i) {
            if (historyCount_ >= tier1Capacity_) {
                evictToArchive();
            } else if (historyCount_ + screenHeight_ >= ringCapacity_) {
                growRing();
            }
            clearPhysicalRow(ringHead_);
            ringHead_ = (ringHead_ + 1) & ringMask();
            historyCount_++;
        }
        markAllDirty();
    } else if (top == 0) {
        // Partial region starting at 0: rows go to history but bottom rows stay put.
        // Save frozen rows below the scroll region.
        int frozenCount = screenHeight_ - bottom;
        std::vector<Cell> frozen(static_cast<size_t>(frozenCount) * cols_);
        std::vector<std::unordered_map<int, CellExtra>> frozenExtras(frozenCount);
        for (int i = 0; i < frozenCount; ++i) {
            int phys = screenRowToPhysical(bottom + i);
            std::memcpy(&frozen[static_cast<size_t>(i) * cols_], rowPtr(phys), cols_ * sizeof(Cell));
            frozenExtras[i] = std::move(ringExtras_[phys]);
        }

        // Advance ring head
        for (int i = 0; i < n; ++i) {
            if (historyCount_ >= tier1Capacity_) evictToArchive();
            else if (historyCount_ + screenHeight_ >= ringCapacity_) growRing();
            clearPhysicalRow(ringHead_);
            ringHead_ = (ringHead_ + 1) & ringMask();
            historyCount_++;
        }

        // Restore frozen rows at their screen positions
        for (int i = 0; i < frozenCount; ++i) {
            int phys = screenRowToPhysical(bottom + i);
            std::memcpy(rowPtr(phys), &frozen[static_cast<size_t>(i) * cols_], cols_ * sizeof(Cell));
            ringExtras_[phys] = std::move(frozenExtras[i]);
        }
        // Clear the gap [bottom-n..bottom-1]
        for (int r = bottom - n; r < bottom; ++r) {
            clearRow(r);
        }
        markAllDirty();
    } else {
        // Scroll region doesn't start at 0: no history, in-place shift.
        for (int r = top; r < bottom - n; ++r) {
            int dstPhys = screenRowToPhysical(r);
            int srcPhys = screenRowToPhysical(r + n);
            std::memcpy(rowPtr(dstPhys), rowPtr(srcPhys), cols_ * sizeof(Cell));
            ringExtras_[dstPhys] = std::move(ringExtras_[srcPhys]);
            markRowDirty(r);
        }
        for (int r = bottom - n; r < bottom; ++r) {
            clearRow(r);
        }
    }
}

void Document::scrollDown(int top, int bottom, int n) {
    if (top < 0 || bottom > screenHeight_ || top >= bottom || n <= 0) return;
    n = std::min(n, bottom - top);

    for (int r = bottom - 1; r >= top + n; --r) {
        int dstPhys = screenRowToPhysical(r);
        int srcPhys = screenRowToPhysical(r - n);
        std::memcpy(rowPtr(dstPhys), rowPtr(srcPhys), cols_ * sizeof(Cell));
        ringExtras_[dstPhys] = std::move(ringExtras_[srcPhys]);
        markRowDirty(r);
    }
    for (int r = top; r < top + n; ++r) {
        clearRow(r);
    }
}

void Document::deleteChars(int screenRow, int col, int count) {
    if (screenRow < 0 || screenRow >= screenHeight_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Cell* r = row(screenRow);
    int remaining = cols_ - col - count;
    if (remaining > 0) std::memmove(&r[col], &r[col + count], remaining * sizeof(Cell));
    for (int c = cols_ - count; c < cols_; ++c) r[c] = Cell{};
    // Shift extras entries left
    int phys = screenRowToPhysical(screenRow);
    auto& ex = ringExtras_[phys];
    if (!ex.empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto& [c, e] : ex) {
            if (c >= col + count) shifted[c - count] = std::move(e);
            else if (c < col) shifted[c] = std::move(e);
        }
        ex = std::move(shifted);
    }
    markRowDirty(screenRow);
}

void Document::insertChars(int screenRow, int col, int count) {
    if (screenRow < 0 || screenRow >= screenHeight_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Cell* r = row(screenRow);
    int remaining = cols_ - col - count;
    if (remaining > 0) std::memmove(&r[col + count], &r[col], remaining * sizeof(Cell));
    for (int c = col; c < col + count; ++c) r[c] = Cell{};
    // Shift extras entries right
    int phys = screenRowToPhysical(screenRow);
    auto& ex = ringExtras_[phys];
    if (!ex.empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto& [c, e] : ex) {
            if (c >= col && c + count < cols_) shifted[c + count] = std::move(e);
            else if (c < col) shifted[c] = std::move(e);
        }
        ex = std::move(shifted);
    }
    markRowDirty(screenRow);
}

// --- Extras ---

const CellExtra* Document::getExtra(int col, int screenRow) const {
    if (screenRow < 0 || screenRow >= screenHeight_) return nullptr;
    int phys = screenRowToPhysical(screenRow);
    auto& m = ringExtras_[phys];
    auto it = m.find(col);
    return (it != m.end()) ? &it->second : nullptr;
}

CellExtra& Document::ensureExtra(int col, int screenRow) {
    assert(screenRow >= 0 && screenRow < screenHeight_);
    int phys = screenRowToPhysical(screenRow);
    return ringExtras_[phys][col];
}

void Document::clearExtra(int col, int screenRow) {
    if (screenRow >= 0 && screenRow < screenHeight_) {
        int phys = screenRowToPhysical(screenRow);
        ringExtras_[phys].erase(col);
    }
}

void Document::clearRowExtras(int screenRow) {
    if (screenRow >= 0 && screenRow < screenHeight_) {
        int phys = screenRowToPhysical(screenRow);
        ringExtras_[phys].clear();
    }
}

// --- History ---

int Document::historySize() const {
    return static_cast<int>(archive_.size()) + historyCount_;
}

const Cell* Document::historyRow(int idx) const {
    if (idx < 0) return nullptr;
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) {
        parseArchivedRow(archive_[idx]);
        return parseBuffer_.data();
    }
    int tier1Idx = idx - archiveSize;
    if (tier1Idx >= historyCount_) return nullptr;
    return rowPtr(historyTier1ToPhysical(tier1Idx));
}

const std::unordered_map<int, CellExtra>* Document::historyExtras(int idx) const {
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) return nullptr;
    int tier1Idx = idx - archiveSize;
    if (tier1Idx < 0 || tier1Idx >= historyCount_) return nullptr;
    int phys = historyTier1ToPhysical(tier1Idx);
    const auto& m = ringExtras_[phys];
    return m.empty() ? nullptr : &m;
}

void Document::clearHistory() {
    archive_.clear();
    historyCount_ = 0;
    parseBuffer_.clear();
}

// --- Viewport ---

const Cell* Document::viewportRow(int viewRow, int viewportOffset) const {
    if (viewportOffset == 0) {
        return row(viewRow);
    }
    int histSize = historySize();
    int logicalRow = histSize - viewportOffset + viewRow;
    if (logicalRow < histSize) {
        return historyRow(logicalRow);
    } else {
        int screenRow = logicalRow - histSize;
        if (screenRow < 0 || screenRow >= screenHeight_) return nullptr;
        return row(screenRow);
    }
}

const std::unordered_map<int, CellExtra>* Document::viewportExtras(int viewRow, int viewportOffset) const {
    if (viewportOffset == 0) {
        int phys = screenRowToPhysical(viewRow);
        const auto& m = ringExtras_[phys];
        return m.empty() ? nullptr : &m;
    }
    int histSize = historySize();
    int logicalRow = histSize - viewportOffset + viewRow;
    if (logicalRow < histSize) {
        return historyExtras(logicalRow);
    } else {
        int screenRow = logicalRow - histSize;
        if (screenRow < 0 || screenRow >= screenHeight_) return nullptr;
        int phys = screenRowToPhysical(screenRow);
        const auto& m = ringExtras_[phys];
        return m.empty() ? nullptr : &m;
    }
}

// --- Continued flag accessors ---

bool Document::isRowContinued(int screenRow) const {
    int phys = screenRowToPhysical(screenRow);
    return continued_[phys];
}

void Document::setRowContinued(int screenRow, bool v) {
    int phys = screenRowToPhysical(screenRow);
    continued_[phys] = v;
}

bool Document::isHistoryRowContinued(int idx) const {
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) return archive_[idx].continued;
    int tier1Idx = idx - archiveSize;
    if (tier1Idx < 0 || tier1Idx >= historyCount_) return false;
    return continued_[historyTier1ToPhysical(tier1Idx)];
}

uint64_t Document::rowIdForAbs(int abs) const {
    if (abs < 0) return 0;
    return rowIdBase_ + static_cast<uint64_t>(abs);
}

int Document::absForRowId(uint64_t id) const {
    if (id < rowIdBase_) return -1;
    uint64_t diff = id - rowIdBase_;
    int total = static_cast<int>(archive_.size()) + historyCount_ + screenHeight_;
    if (diff >= static_cast<uint64_t>(total)) return -1;
    return static_cast<int>(diff);
}

std::string Document::getTextFromRows(uint64_t startRowId, uint64_t endRowId,
                                       int startCol, int endCol) const
{
    int startAbs = absForRowId(startRowId);
    int endAbs   = absForRowId(endRowId);
    if (startAbs < 0) return {};
    if (endAbs < 0) {
        // End row evicted — clamp to last available row.
        int total = static_cast<int>(archive_.size()) + historyCount_ + screenHeight_;
        endAbs = total - 1;
    }
    if (startAbs > endAbs) return {};
    endAbs = std::min(endAbs, static_cast<int>(archive_.size()) + historyCount_ + screenHeight_ - 1);

    std::string out;
    int archSz  = static_cast<int>(archive_.size());
    int histEnd = archSz + historyCount_;
    int scrEnd  = histEnd + screenHeight_;

    for (int abs = startAbs; abs <= endAbs; ++abs) {
        const Cell* row = nullptr;
        if (abs < archSz) {
            parseArchivedRow(archive_[abs]);
            row = parseBuffer_.data();
        } else if (abs < histEnd) {
            row = rowPtr(historyTier1ToPhysical(abs - archSz));
        } else if (abs < scrEnd) {
            row = rowPtr(screenRowToPhysical(abs - histEnd));
        }
        if (!row) continue;

        int colStart = (abs == startAbs) ? std::max(0, startCol) : 0;
        int colEnd   = (abs == endAbs)   ? std::min(cols_, endCol == std::numeric_limits<int>::max() ? cols_ : endCol) : cols_;

        // Trim trailing blanks.
        int effective = colEnd;
        while (effective > colStart && row[effective - 1].wc == 0) --effective;

        for (int c = colStart; c < effective; ++c) {
            char32_t ch = row[c].wc ? row[c].wc : U' ';
            char buf[4]; int n = 0;
            if      (ch < 0x80)    { buf[n++] = static_cast<char>(ch); }
            else if (ch < 0x800)   { buf[n++] = static_cast<char>(0xC0 | (ch >> 6));
                                     buf[n++] = static_cast<char>(0x80 | (ch & 0x3F)); }
            else if (ch < 0x10000) { buf[n++] = static_cast<char>(0xE0 | (ch >> 12));
                                     buf[n++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                                     buf[n++] = static_cast<char>(0x80 | (ch & 0x3F)); }
            else                   { buf[n++] = static_cast<char>(0xF0 | (ch >> 18));
                                     buf[n++] = static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                                     buf[n++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                                     buf[n++] = static_cast<char>(0x80 | (ch & 0x3F)); }
            out.append(buf, n);
        }
        if (abs < endAbs) out += '\n';
    }
    return out;
}

// --- Resize ---

void Document::resize(int newCols, int newRows, CursorTrack* cursor) {
    if (newCols == cols_ && newRows == screenHeight_) return;

    // Handle initial construction (empty document, from default constructor path)
    if (ringCapacity_ == 0) {
        cols_ = newCols;
        screenHeight_ = newRows;
        int initCap = roundUpPow2(tier1Capacity_ + screenHeight_ + SEG_SIZE);
        ringCapacity_ = initCap;
        allocSegments(0, ringCapacity_ >> SEG_SHIFT);
        for (Cell* seg : segments_) {
            std::memset(seg, 0, static_cast<size_t>(SEG_SIZE) * cols_ * sizeof(Cell));
        }
        ringExtras_.resize(ringCapacity_);
        continued_.assign(ringCapacity_, false);
        dirty_.assign(screenHeight_, true);
        allDirty_ = true;
        ringHead_ = screenHeight_;
        historyCount_ = 0;
        return;
    }

    if (newCols != cols_) {
        // === Column change: full reflow ===

        // Step 1: Collect all source rows in order (archive → tier-1 history → screen)
        // Trim trailing empty screen rows (below cursor) — they're just padding
        int archiveSize = static_cast<int>(archive_.size());
        int usedScreenRows = screenHeight_;
        if (cursor) {
            int cursorScreenRow = cursor->srcY - (archiveSize + historyCount_);
            usedScreenRows = std::max(1, std::min(screenHeight_, cursorScreenRow + 1));
        }
        int cursorLimit = cursor ? (cursor->srcY - (archiveSize + historyCount_) + 1) : 0;
        while (usedScreenRows > cursorLimit) {
            int phys = screenRowToPhysical(usedScreenRows - 1);
            const Cell* r = rowPtr(phys);
            bool blank = true;
            for (int c = 0; c < cols_ && blank; ++c) {
                if (r[c].wc != 0) blank = false;
            }
            if (!blank || continued_[phys]) break;
            usedScreenRows--;
        }
        if (usedScreenRows < 1 && (historyCount_ > 0 || archiveSize > 0)) usedScreenRows = 0;
        else if (usedScreenRows < 1) usedScreenRows = 1;
        int totalSrcRows = archiveSize + historyCount_ + usedScreenRows;

        struct SrcRow {
            const Cell* cells;
            int cols;
            bool cont;
            const std::unordered_map<int, CellExtra>* extras;
        };

        std::vector<std::vector<Cell>> archivedCells(archiveSize);
        auto getSrcRow = [&](int idx) -> SrcRow {
            if (idx < archiveSize) {
                if (archivedCells[idx].empty()) {
                    parseArchivedRow(archive_[idx]);
                    archivedCells[idx].assign(parseBuffer_.begin(), parseBuffer_.end());
                }
                return {archivedCells[idx].data(), cols_, archive_[idx].continued, nullptr};
            }
            int ringIdx = idx - archiveSize;
            int phys;
            if (ringIdx < historyCount_) {
                phys = historyTier1ToPhysical(ringIdx);
            } else {
                int screenRow = ringIdx - historyCount_;
                phys = screenRowToPhysical(screenRow);
            }
            const auto& ex = ringExtras_[phys];
            return {rowPtr(phys), cols_, continued_[phys], ex.empty() ? nullptr : &ex};
        };

        // Step 2 & 3: Join logical lines and re-wrap at new width, tracking cursor
        std::vector<Cell> dstCells;
        std::vector<bool> dstContinued;
        std::vector<std::unordered_map<int, CellExtra>> dstExtras;

        int dstRow = 0;
        int dstCol = 0;

        auto startNewDstRow = [&]() {
            dstCells.resize(static_cast<size_t>(dstRow + 1) * newCols);
            dstExtras.resize(dstRow + 1);
            dstContinued.resize(dstRow + 1, false);
        };

        auto finishDstRow = [&](bool cont) {
            dstContinued[dstRow] = cont;
            dstRow++;
            dstCol = 0;
        };

        startNewDstRow();

        int srcIdx = 0;
        while (srcIdx < totalSrcRows) {
            int logStart = srcIdx;
            while (srcIdx < totalSrcRows - 1 && getSrcRow(srcIdx).cont) {
                srcIdx++;
            }
            int logEnd = srcIdx;
            srcIdx++;

            for (int ri = logStart; ri <= logEnd; ++ri) {
                SrcRow sr = getSrcRow(ri);
                int effectiveWidth = sr.cols;
                if (ri == logEnd) {
                    bool hasImageExtra = false;
                    if (sr.extras) {
                        for (auto& [col, ex] : *sr.extras) {
                            if (ex.imageId != 0) { hasImageExtra = true; break; }
                        }
                    }
                    if (!hasImageExtra) {
                        while (effectiveWidth > 0) {
                            const Cell& c = sr.cells[effectiveWidth - 1];
                            if (c.wc != 0 || c.attrs.fgMode() != CellAttrs::Default || c.attrs.bgMode() != CellAttrs::Default
                                || c.attrs.bold() || c.attrs.italic() || c.attrs.underline()) break;
                            effectiveWidth--;
                        }
                    }
                }

                for (int sc = 0; sc < effectiveWidth; ++sc) {
                    const Cell& cell = sr.cells[sc];

                    if (cell.attrs.wide() && dstCol == newCols - 1) {
                        dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = Cell{' ', CellAttrs{}};
                        finishDstRow(true);
                        startNewDstRow();
                    }

                    if (cell.attrs.wideSpacer()) continue;

                    if (cursor && ri == cursor->srcY && sc == cursor->srcX) {
                        cursor->dstY = dstRow;
                        cursor->dstX = dstCol;
                    }

                    dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = cell;

                    if (sr.extras) {
                        auto eit = sr.extras->find(sc);
                        if (eit != sr.extras->end()) {
                            dstExtras[dstRow][dstCol] = eit->second;
                        }
                    }

                    dstCol++;

                    if (cell.attrs.wide() && dstCol < newCols) {
                        CellAttrs spacerAttrs{};
                        spacerAttrs.setWideSpacer(true);
                        dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = Cell{0, spacerAttrs};
                        dstCol++;
                    }

                    if (dstCol >= newCols) {
                        bool moreContent = (sc + 1 < effectiveWidth) || (ri < logEnd);
                        if (moreContent) {
                            finishDstRow(true);
                            startNewDstRow();
                        } else {
                            dstCol = newCols;
                        }
                    }
                }

                if (cursor && ri == cursor->srcY && cursor->srcX >= effectiveWidth) {
                    cursor->dstY = dstRow;
                    cursor->dstX = dstCol;
                }
            }

            if (dstCol > 0 || dstRow == 0) {
                finishDstRow(false);
                startNewDstRow();
            } else if (dstCol == 0 && dstRow > 0 && !dstContinued[dstRow - 1]) {
                finishDstRow(false);
                startNewDstRow();
            } else if (dstCol >= newCols) {
                dstCol = 0;
                finishDstRow(false);
                startNewDstRow();
            }
        }

        int totalDstRows = dstRow;

        // Step 4: Install into ring
        int newScreenRows = newRows;
        int newHistoryCount = std::max(0, totalDstRows - newScreenRows);

        int archiveEvict = newHistoryCount - tier1Capacity_;
        if (archiveEvict > 0) {
            for (int i = 0; i < archiveEvict && i < newHistoryCount; ++i) {
                Cell* rowCells = &dstCells[static_cast<size_t>(i) * newCols];
                archive_.push_back({serializeRow(rowCells, newCols), dstContinued[i]});
                if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
                    archive_.pop_front();
                    ++rowIdBase_;
                }
            }
            if (cursor) cursor->dstY -= archiveEvict;
            newHistoryCount -= archiveEvict;
            int keepStart = archiveEvict;
            int keepCount = totalDstRows - archiveEvict;
            std::vector<Cell> trimmedCells(static_cast<size_t>(keepCount) * newCols);
            std::memcpy(trimmedCells.data(), &dstCells[static_cast<size_t>(keepStart) * newCols],
                        static_cast<size_t>(keepCount) * newCols * sizeof(Cell));
            dstCells = std::move(trimmedCells);
            std::vector<bool> trimmedCont(dstContinued.begin() + keepStart, dstContinued.end());
            dstContinued = std::move(trimmedCont);
            std::vector<std::unordered_map<int, CellExtra>> trimmedExtras(
                std::make_move_iterator(dstExtras.begin() + keepStart),
                std::make_move_iterator(dstExtras.end()));
            dstExtras = std::move(trimmedExtras);
            totalDstRows = keepCount;
        }

        int ringTotal = newHistoryCount + newScreenRows;
        int newCap = (tier1Capacity_ < std::numeric_limits<int>::max() / 2)
            ? roundUpPow2(std::max(ringTotal, tier1Capacity_ + newRows) + SEG_SIZE)
            : roundUpPow2(ringTotal + SEG_SIZE);

        // Reallocate segments for new column width. Zero-init all segments so
        // screen rows not populated from dstCells below contain valid blank cells.
        freeSegments();
        cols_ = newCols;
        allocSegments(0, newCap >> SEG_SHIFT);
        for (Cell* seg : segments_) {
            std::memset(seg, 0, static_cast<size_t>(SEG_SIZE) * cols_ * sizeof(Cell));
        }
        ringExtras_.assign(newCap, {});
        continued_.assign(newCap, false);

        for (int i = 0; i < ringTotal && i < totalDstRows; ++i) {
            std::memcpy(rowPtr(i), &dstCells[static_cast<size_t>(i) * newCols], newCols * sizeof(Cell));
            ringExtras_[i] = std::move(dstExtras[i]);
            continued_[i] = dstContinued[i];
        }

        ringCapacity_ = newCap;
        ringHead_ = ringTotal;
        historyCount_ = newHistoryCount;
        screenHeight_ = newRows;

    } else if (newRows != screenHeight_) {
        // Height-only change: no reflow needed
        if (newRows < screenHeight_) {
            int delta = screenHeight_ - newRows;
            int cursorScreenRow = cursor ? (cursor->srcY - historySize()) : -1;
            int blanksAtBottom = 0;
            for (int r = screenHeight_ - 1; r >= 0 && blanksAtBottom < delta; --r) {
                if (r == cursorScreenRow) break;
                int phys = screenRowToPhysical(r);
                const Cell* rp = rowPtr(phys);
                bool blank = true;
                for (int c = 0; c < cols_ && blank; ++c) {
                    if (rp[c].wc != 0) blank = false;
                }
                if (!blank) break;
                blanksAtBottom++;
            }
            ringHead_ = (ringHead_ - blanksAtBottom + ringCapacity_) & ringMask();
            int pushToHistory = delta - blanksAtBottom;
            historyCount_ += pushToHistory;
            while (historyCount_ > tier1Capacity_) {
                evictToArchive();
            }
        } else {
            int delta = newRows - screenHeight_;
            int needed = historyCount_ + newRows;
            if (needed > ringCapacity_) {
                int newCap = roundUpPow2(needed + SEG_SIZE);
                growRingGeneral(newCap);
            }
            for (int i = 0; i < delta; ++i) {
                clearPhysicalRow(ringHead_);
                ringHead_ = (ringHead_ + 1) & ringMask();
            }
        }
        screenHeight_ = newRows;

        int needed = historyCount_ + screenHeight_;
        if (needed > ringCapacity_) {
            int newCap = roundUpPow2(needed + SEG_SIZE);
            growRingGeneral(newCap);
        }
    }

    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
}

// --- Serialization ---

std::string Document::serializeRow(const Cell* cells, int cols) {
    std::string out;
    out.reserve(cols * 2);
    CellAttrs currentAttrs{};
    auto emitOsc133 = [&](CellAttrs::SemanticType t) {
        char verb = '\0';
        switch (t) {
            case CellAttrs::Prompt: verb = 'A'; break;
            case CellAttrs::Input:  verb = 'B'; break;
            case CellAttrs::Output: verb = 'C'; break;
        }
        if (verb) {
            out += '\x1b'; out += ']'; out += '1'; out += '3'; out += '3';
            out += ';'; out += verb;
            out += '\x1b'; out += '\\';
        }
    };
    for (int c = 0; c < cols; ++c) {
        const Cell& cell = cells[c];
        if (cell.attrs.semanticType() != currentAttrs.semanticType()) {
            emitOsc133(cell.attrs.semanticType());
        }
        if (std::memcmp(cell.attrs.data, currentAttrs.data, 8) != 0) {
            emitSGR(out, currentAttrs, cell.attrs);
            currentAttrs = cell.attrs;
        }
        if (cell.wc == 0) {
            out += ' ';
        } else {
            char buf[4];
            int n = utf8::encode(cell.wc, buf);
            out.append(buf, n);
        }
    }
    return out;
}

void Document::parseArchivedRow(const ArchivedRow& archived) const {
    parseBuffer_.resize(cols_);
    for (int c = 0; c < cols_; ++c) parseBuffer_[c] = Cell{};

    const char* s = archived.data.c_str();
    int len = static_cast<int>(archived.data.size());
    int pos = 0;
    int col = 0;
    CellAttrs currentAttrs{};

    while (pos < len && col < cols_) {
        if (s[pos] == '\x1b' && pos + 1 < len && s[pos + 1] == '[') {
            // SGR escape
            int start = pos + 2;
            int end = start;
            while (end < len && s[end] != 'm') ++end;
            if (end < len) {
                parseSGRParams(s + start, end - start, currentAttrs);
                pos = end + 1;
            } else {
                pos = len;
            }
        } else if (s[pos] == '\x1b' && pos + 1 < len && s[pos + 1] == ']') {
            // OSC escape
            int start = pos + 2;
            int end = start;
            while (end + 1 < len && !(s[end] == '\x1b' && s[end + 1] == '\\')) ++end;
            if (end - start >= 5 && s[start] == '1' && s[start + 1] == '3' &&
                s[start + 2] == '3' && s[start + 3] == ';') {
                switch (s[start + 4]) {
                    case 'A': case 'N': case 'P':
                        currentAttrs.setSemanticType(CellAttrs::Prompt); break;
                    case 'B': case 'I':
                        currentAttrs.setSemanticType(CellAttrs::Input); break;
                    case 'C': case 'D':
                        currentAttrs.setSemanticType(CellAttrs::Output); break;
                    default: break;
                }
            }
            pos = (end + 1 < len) ? end + 2 : len;
        } else {
            int consumed;
            char32_t cp = utf8::decode(s + pos, len - pos, consumed);
            parseBuffer_[col].wc = (cp == ' ') ? 0 : cp;
            parseBuffer_[col].attrs = currentAttrs;
            col++;
            pos += consumed;
        }
    }
}
