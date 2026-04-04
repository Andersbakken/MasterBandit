#include "Document.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <cstring>

// --- SGR helpers (copied from ScrollbackBuffer.cpp) ---

static void emitSGR(std::string& out, const CellAttrs& oldA, const CellAttrs& newA) {
    bool needReset = (oldA.bold() && !newA.bold()) || (oldA.italic() && !newA.italic()) ||
                     (oldA.underline() && !newA.underline()) || (oldA.strikethrough() && !newA.strikethrough()) ||
                     (oldA.blink() && !newA.blink()) || (oldA.inverse() && !newA.inverse()) ||
                     (oldA.dim() && !newA.dim()) || (oldA.invisible() && !newA.invisible());

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

Document::Document() = default;

Document::Document(int cols, int screenHeight, int tier1Capacity, int maxArchiveRows)
    : cols_(cols)
    , screenHeight_(screenHeight)
    , maxArchiveRows_(maxArchiveRows)
    , tier1Capacity_(tier1Capacity)
{
    // Start the ring at a modest size; it grows dynamically as history accumulates.
    // Cap the initial allocation to avoid overflow when tier1Capacity is INT_MAX (infinite).
    int initialHistory = std::min(tier1Capacity, 1024);
    ringCapacity_ = roundUpPow2(initialHistory + screenHeight + 64);
    ring_.resize(static_cast<size_t>(ringCapacity_) * cols_);
    ringExtras_.resize(ringCapacity_);
    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
    ringHead_ = screenHeight_; // screen rows are [0..screenHeight_-1], head is past them
    historyCount_ = 0;
}

int Document::screenRowToPhysical(int screenRow) const {
    return (ringHead_ - screenHeight_ + screenRow) & ringMask();
}

int Document::historyTier1ToPhysical(int tier1Idx) const {
    return (ringHead_ - screenHeight_ - historyCount_ + tier1Idx) & ringMask();
}

void Document::clearPhysicalRow(int physical) {
    Cell* r = &ring_[static_cast<size_t>(physical) * cols_];
    for (int c = 0; c < cols_; ++c) r[c] = Cell{};
    ringExtras_[physical].clear();
}

void Document::evictToArchive() {
    if (historyCount_ <= 0) return;
    int oldest = historyTier1ToPhysical(0);
    archive_.push_back({serializeRow(&ring_[static_cast<size_t>(oldest) * cols_], cols_)});
    ringExtras_[oldest].clear();
    if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
        archive_.pop_front();
    }
    historyCount_--;
}

// --- IGrid implementation ---

Cell& Document::cell(int col, int screenRow) {
    int phys = screenRowToPhysical(screenRow);
    return ring_[static_cast<size_t>(phys) * cols_ + col];
}

const Cell& Document::cell(int col, int screenRow) const {
    int phys = screenRowToPhysical(screenRow);
    return ring_[static_cast<size_t>(phys) * cols_ + col];
}

Cell* Document::row(int screenRow) {
    int phys = screenRowToPhysical(screenRow);
    return &ring_[static_cast<size_t>(phys) * cols_];
}

const Cell* Document::row(int screenRow) const {
    int phys = screenRowToPhysical(screenRow);
    return &ring_[static_cast<size_t>(phys) * cols_];
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
    Cell* r = &ring_[static_cast<size_t>(phys) * cols_];
    for (int c = 0; c < cols_; ++c) r[c] = Cell{};
    ringExtras_[phys].clear();
    markRowDirty(screenRow);
}

void Document::clearRow(int screenRow, int startCol, int endCol) {
    if (screenRow < 0 || screenRow >= screenHeight_) return;
    startCol = std::max(0, startCol);
    endCol = std::min(cols_, endCol);
    int phys = screenRowToPhysical(screenRow);
    Cell* r = &ring_[static_cast<size_t>(phys) * cols_];
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
            // Ensure ring has space
            if (historyCount_ >= tier1Capacity_) {
                evictToArchive();
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
            std::memcpy(&frozen[static_cast<size_t>(i) * cols_],
                        &ring_[static_cast<size_t>(phys) * cols_], cols_ * sizeof(Cell));
            frozenExtras[i] = std::move(ringExtras_[phys]);
        }

        // Advance ring head
        for (int i = 0; i < n; ++i) {
            if (historyCount_ >= tier1Capacity_) evictToArchive();
            clearPhysicalRow(ringHead_);
            ringHead_ = (ringHead_ + 1) & ringMask();
            historyCount_++;
        }

        // Restore frozen rows at their screen positions
        for (int i = 0; i < frozenCount; ++i) {
            int phys = screenRowToPhysical(bottom + i);
            std::memcpy(&ring_[static_cast<size_t>(phys) * cols_],
                        &frozen[static_cast<size_t>(i) * cols_], cols_ * sizeof(Cell));
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
            std::memcpy(&ring_[static_cast<size_t>(dstPhys) * cols_],
                        &ring_[static_cast<size_t>(srcPhys) * cols_], cols_ * sizeof(Cell));
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
        std::memcpy(&ring_[static_cast<size_t>(dstPhys) * cols_],
                    &ring_[static_cast<size_t>(srcPhys) * cols_], cols_ * sizeof(Cell));
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
    markRowDirty(screenRow);
}

void Document::insertChars(int screenRow, int col, int count) {
    if (screenRow < 0 || screenRow >= screenHeight_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Cell* r = row(screenRow);
    int remaining = cols_ - col - count;
    if (remaining > 0) std::memmove(&r[col + count], &r[col], remaining * sizeof(Cell));
    for (int c = col; c < col + count; ++c) r[c] = Cell{};
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
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) {
        parseArchivedRow(archive_[idx]);
        return parseBuffer_.data();
    }
    int tier1Idx = idx - archiveSize;
    if (tier1Idx < 0 || tier1Idx >= historyCount_) return nullptr;
    int phys = historyTier1ToPhysical(tier1Idx);
    return &ring_[static_cast<size_t>(phys) * cols_];
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

// --- Resize ---

void Document::resize(int newCols, int newRows) {
    if (newCols == cols_ && newRows == screenHeight_) return;

    // Handle initial construction (empty document)
    if (ringCapacity_ == 0) {
        cols_ = newCols;
        screenHeight_ = newRows;
        ringCapacity_ = roundUpPow2(tier1Capacity_ + screenHeight_ + 64);
        ring_.resize(static_cast<size_t>(ringCapacity_) * cols_);
        ringExtras_.resize(ringCapacity_);
        dirty_.assign(screenHeight_, true);
        allDirty_ = true;
        ringHead_ = screenHeight_;
        historyCount_ = 0;
        return;
    }

    if (newCols != cols_) {
        // Column change: rebuild ring rows to new width
        int totalRows = historyCount_ + screenHeight_;
        std::vector<Cell> newRing(static_cast<size_t>(ringCapacity_) * newCols);
        std::vector<std::unordered_map<int, CellExtra>> newExtras(ringCapacity_);
        int minCols = std::min(cols_, newCols);

        for (int i = 0; i < totalRows; ++i) {
            int oldPhys = (ringHead_ - totalRows + i) & ringMask();
            int newPhys = i;
            std::memcpy(&newRing[static_cast<size_t>(newPhys) * newCols],
                        &ring_[static_cast<size_t>(oldPhys) * cols_],
                        minCols * sizeof(Cell));
            if (newCols > cols_) {
                std::memset(&newRing[static_cast<size_t>(newPhys) * newCols + cols_], 0,
                            (newCols - cols_) * sizeof(Cell));
            }
            newExtras[newPhys] = std::move(ringExtras_[oldPhys]);
        }
        ring_ = std::move(newRing);
        ringExtras_ = std::move(newExtras);
        ringHead_ = totalRows & ringMask();
        cols_ = newCols;
    }

    if (newRows != screenHeight_) {
        if (newRows < screenHeight_) {
            // Shrink: top screen rows become history
            int delta = screenHeight_ - newRows;
            historyCount_ += delta;
            while (historyCount_ > tier1Capacity_) {
                evictToArchive();
            }
        } else {
            // Grow: pull rows from history into screen
            int delta = newRows - screenHeight_;
            int pull = std::min(delta, historyCount_);
            historyCount_ -= pull;
            // New rows beyond history are already zeroed in ring
        }
        screenHeight_ = newRows;
    }

    // Ensure ring capacity
    int needed = historyCount_ + screenHeight_;
    if (needed > ringCapacity_) {
        int newCap = roundUpPow2(needed + 64);
        std::vector<Cell> newRing(static_cast<size_t>(newCap) * cols_);
        std::vector<std::unordered_map<int, CellExtra>> newExtras(newCap);
        int total = historyCount_ + screenHeight_;
        for (int i = 0; i < total; ++i) {
            int oldPhys = (ringHead_ - total + i) & ringMask();
            std::memcpy(&newRing[static_cast<size_t>(i) * cols_],
                        &ring_[static_cast<size_t>(oldPhys) * cols_], cols_ * sizeof(Cell));
            newExtras[i] = std::move(ringExtras_[oldPhys]);
        }
        ring_ = std::move(newRing);
        ringExtras_ = std::move(newExtras);
        ringCapacity_ = newCap;
        ringHead_ = total & (newCap - 1);
    }

    dirty_.assign(screenHeight_, true);
    allDirty_ = true;
}

// --- Serialization (from ScrollbackBuffer) ---

std::string Document::serializeRow(const Cell* cells, int cols) {
    std::string out;
    out.reserve(cols * 2);
    CellAttrs currentAttrs{};
    for (int c = 0; c < cols; ++c) {
        const Cell& cell = cells[c];
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
