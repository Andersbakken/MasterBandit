#include "Document.h"
#include "Utf8.h"
#include <algorithm>
#include <cassert>
#include <climits>
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
    continued_.assign(ringCapacity_, false);
    promptKind_.assign(ringCapacity_, UnknownPrompt);
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
    continued_[physical] = false;
    promptKind_[physical] = UnknownPrompt;
}

void Document::evictToArchive() {
    if (historyCount_ <= 0) return;
    int oldest = historyTier1ToPhysical(0);
    archive_.push_back({serializeRow(&ring_[static_cast<size_t>(oldest) * cols_], cols_), continued_[oldest]});
    ringExtras_[oldest].clear();
    continued_[oldest] = false;
    if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
        archive_.pop_front();
    }
    historyCount_--;
}

void Document::growRing()
{
    int total = historyCount_ + screenHeight_;
    int newCap = roundUpPow2(total + 64);
    // Cap at tier1Capacity_ + screenHeight_ + padding, but only when tier1Capacity_
    // is finite — adding to INT_MAX overflows.
    if (tier1Capacity_ < INT_MAX / 2) {
        int maxCap = roundUpPow2(tier1Capacity_ + screenHeight_ + 64);
        newCap = std::min(newCap, maxCap);
    }
    if (newCap <= ringCapacity_) return; // already large enough

    std::vector<Cell> newRing(static_cast<size_t>(newCap) * cols_);
    std::vector<std::unordered_map<int, CellExtra>> newExtras(newCap);
    std::vector<bool> newCont(newCap, false);
    std::vector<PromptKind> newPK(newCap, UnknownPrompt);
    for (int i = 0; i < total; ++i) {
        int oldPhys = (ringHead_ - total + i) & ringMask();
        std::memcpy(&newRing[static_cast<size_t>(i) * cols_],
                    &ring_[static_cast<size_t>(oldPhys) * cols_], cols_ * sizeof(Cell));
        newExtras[i] = std::move(ringExtras_[oldPhys]);
        newCont[i] = continued_[oldPhys];
        newPK[i] = promptKind_[oldPhys];
    }
    ring_ = std::move(newRing);
    ringExtras_ = std::move(newExtras);
    continued_ = std::move(newCont);
    promptKind_ = std::move(newPK);
    ringCapacity_ = newCap;
    ringHead_ = total & (newCap - 1);
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
            std::memcpy(&frozen[static_cast<size_t>(i) * cols_],
                        &ring_[static_cast<size_t>(phys) * cols_], cols_ * sizeof(Cell));
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


Document::PromptKind Document::rowPromptKind(int screenRow) const {
    int phys = screenRowToPhysical(screenRow);
    return promptKind_[phys];
}

void Document::setRowPromptKind(int screenRow, PromptKind kind) {
    int phys = screenRowToPhysical(screenRow);
    promptKind_[phys] = kind;
}

Document::PromptKind Document::historyRowPromptKind(int idx) const {
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) return UnknownPrompt; // archive doesn't store prompt kind
    int tier1Idx = idx - archiveSize;
    if (tier1Idx < 0 || tier1Idx >= historyCount_) return UnknownPrompt;
    return promptKind_[historyTier1ToPhysical(tier1Idx)];
}

// --- Resize ---

void Document::resize(int newCols, int newRows, CursorTrack* cursor) {
    if (newCols == cols_ && newRows == screenHeight_) return;

    // Handle initial construction (empty document)
    if (ringCapacity_ == 0) {
        cols_ = newCols;
        screenHeight_ = newRows;
        ringCapacity_ = roundUpPow2(tier1Capacity_ + screenHeight_ + 64);
        ring_.resize(static_cast<size_t>(ringCapacity_) * cols_);
        ringExtras_.resize(ringCapacity_);
        continued_.assign(ringCapacity_, false);
        promptKind_.assign(ringCapacity_, UnknownPrompt);
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
            // Keep at least up to cursor row + 1
            int cursorScreenRow = cursor->srcY - (archiveSize + historyCount_);
            usedScreenRows = std::max(1, std::min(screenHeight_, cursorScreenRow + 1));
        }
        // Also trim any trailing blank rows above the cursor-based limit
        while (usedScreenRows > 0) {
            int phys = screenRowToPhysical(usedScreenRows - 1);
            const Cell* r = &ring_[static_cast<size_t>(phys) * cols_];
            bool blank = true;
            for (int c = 0; c < cols_ && blank; ++c) {
                if (r[c].wc != 0) blank = false;
            }
            if (!blank || continued_[phys]) break;
            usedScreenRows--;
        }
        if (usedScreenRows < 1 && (historyCount_ > 0 || archiveSize > 0)) usedScreenRows = 0;
        else if (usedScreenRows < 1) usedScreenRows = 1; // keep at least one row for empty terminal
        int totalSrcRows = archiveSize + historyCount_ + usedScreenRows;

        // Helper to get source row cells + continued flag
        struct SrcRow {
            const Cell* cells;
            int cols;
            bool cont;
            const std::unordered_map<int, CellExtra>* extras;
            PromptKind promptKind;
        };

        // We need to parse archived rows into temporary buffers
        std::vector<std::vector<Cell>> archivedCells(archiveSize);
        auto getSrcRow = [&](int idx) -> SrcRow {
            if (idx < archiveSize) {
                if (archivedCells[idx].empty()) {
                    parseArchivedRow(archive_[idx]);
                    archivedCells[idx].assign(parseBuffer_.begin(), parseBuffer_.end());
                }
                return {archivedCells[idx].data(), cols_, archive_[idx].continued, nullptr,
                        UnknownPrompt};
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
            return {&ring_[static_cast<size_t>(phys) * cols_], cols_, continued_[phys],
                    ex.empty() ? nullptr : &ex, promptKind_[phys]};
        };

        // Step 2 & 3: Join logical lines and re-wrap at new width, tracking cursor

        // Destination rows built into flat vectors
        std::vector<Cell> dstCells;       // flat: row0[0..newCols-1], row1[0..newCols-1], ...
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
            // Gather one logical line (consecutive rows where all but last are continued)
            int logStart = srcIdx;
            while (srcIdx < totalSrcRows - 1 && getSrcRow(srcIdx).cont) {
                srcIdx++;
            }
            int logEnd = srcIdx; // inclusive
            srcIdx++;

            // Compute effective width of each source row in this logical line (trim trailing blanks).
            // Only trim the last row — continued rows need their full width preserved
            // so that gaps (e.g. between left prompt and rprompt) aren't collapsed.
            // Exception: for prompt lines (OSC 133 PromptStart), also strip the rprompt region
            // from the last row so it doesn't wrap incorrectly on resize.
            const bool isPromptLine = (getSrcRow(logStart).promptKind == PromptStart);

            for (int ri = logStart; ri <= logEnd; ++ri) {
                SrcRow sr = getSrcRow(ri);
                int effectiveWidth = sr.cols;
                if (ri == logEnd) {
                    // Standard trailing-blank trim
                    while (effectiveWidth > 0) {
                        const Cell& c = sr.cells[effectiveWidth - 1];
                        if (c.wc != 0 || c.attrs.fgMode() != CellAttrs::Default || c.attrs.bgMode() != CellAttrs::Default
                            || c.attrs.bold() || c.attrs.italic() || c.attrs.underline()) break;
                        effectiveWidth--;
                    }
                    // For prompt lines: detect and strip the rprompt region.
                    // Pattern: [left-prompt-content][≥3 spaces][rprompt-content] at end of row.
                    // The shell redraws the current prompt after SIGWINCH; for history prompts
                    // this prevents the rprompt from wrapping into extra rows.
                    if (isPromptLine && effectiveWidth > 0) {
                        // Scan backwards past rprompt content (non-blank at end)
                        int pos = effectiveWidth - 1;
                        while (pos >= 0) {
                            char32_t wc = sr.cells[pos].wc;
                            if (wc == 0 || wc == ' ') break;
                            pos--;
                        }
                        // pos now points to the last space/blank before the rprompt
                        if (pos > 0 && (sr.cells[pos].wc == 0 || sr.cells[pos].wc == ' ')) {
                            int gapEnd = pos;
                            while (pos >= 0 && (sr.cells[pos].wc == 0 || sr.cells[pos].wc == ' ')) pos--;
                            int gapStart = pos + 1;
                            if (gapEnd - gapStart >= 3) {
                                // Found a gap of ≥3 spaces — treat gapStart as the effective end
                                effectiveWidth = gapStart;
                            }
                        }
                    }
                }

                // Copy cells from this source row
                for (int sc = 0; sc < effectiveWidth; ++sc) {
                    const Cell& cell = sr.cells[sc];

                    // Handle wide character at destination boundary
                    if (cell.attrs.wide() && dstCol == newCols - 1) {
                        // Can't fit wide char starting at last column — pad and wrap
                        dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = Cell{' ', CellAttrs{}};
                        finishDstRow(true);
                        startNewDstRow();
                    }

                    // Skip wide spacers — they'll be regenerated
                    if (cell.attrs.wideSpacer()) continue;

                    // Track cursor
                    if (cursor && ri == cursor->srcY && sc == cursor->srcX) {
                        cursor->dstY = dstRow;
                        cursor->dstX = dstCol;
                    }

                    // Place the cell
                    dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = cell;

                    // Copy extras if present
                    if (sr.extras) {
                        auto eit = sr.extras->find(sc);
                        if (eit != sr.extras->end()) {
                            dstExtras[dstRow][dstCol] = eit->second;
                        }
                    }

                    dstCol++;

                    // Place wide spacer
                    if (cell.attrs.wide() && dstCol < newCols) {
                        CellAttrs spacerAttrs{};
                        spacerAttrs.setWideSpacer(true);
                        dstCells[static_cast<size_t>(dstRow) * newCols + dstCol] = Cell{0, spacerAttrs};
                        dstCol++;
                    }

                    // Wrap if destination row is full
                    if (dstCol >= newCols) {
                        bool moreContent = (sc + 1 < effectiveWidth) || (ri < logEnd);
                        if (moreContent) {
                            finishDstRow(true);
                            startNewDstRow();
                        } else {
                            // Exactly at boundary, no more content — don't create empty trailing row
                            dstCol = newCols; // will be reset when next logical line starts
                        }
                    }
                }

                // Track cursor if it was past the effective content on this source row
                if (cursor && ri == cursor->srcY && cursor->srcX >= effectiveWidth) {
                    cursor->dstY = dstRow;
                    cursor->dstX = dstCol;
                }
            }

            // End of logical line — finish current row (not continued)
            if (dstCol > 0 || dstRow == 0) {
                finishDstRow(false);
                startNewDstRow();
            } else if (dstCol == 0 && dstRow > 0 && !dstContinued[dstRow - 1]) {
                // Empty logical line — emit one blank row
                finishDstRow(false);
                startNewDstRow();
            } else if (dstCol >= newCols) {
                // Row was exactly full at boundary — already finished
                dstCol = 0;
                finishDstRow(false);
                startNewDstRow();
            }
        }

        // Remove the trailing empty row from startNewDstRow
        int totalDstRows = dstRow;

        // Step 4: Install into ring
        int newScreenRows = newRows;
        int newHistoryCount = std::max(0, totalDstRows - newScreenRows);

        // Evict excess history to archive
        int archiveEvict = newHistoryCount - tier1Capacity_;
        if (archiveEvict > 0) {
            for (int i = 0; i < archiveEvict && i < newHistoryCount; ++i) {
                Cell* rowCells = &dstCells[static_cast<size_t>(i) * newCols];
                archive_.push_back({serializeRow(rowCells, newCols), dstContinued[i]});
                if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
                    archive_.pop_front();
                }
            }
            // Shift cursor tracking
            if (cursor) cursor->dstY -= archiveEvict;
            newHistoryCount -= archiveEvict;
            // Shift dst data: remove the evicted rows from the beginning
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
        // Avoid overflow when tier1Capacity_ is INT_MAX (infinite scrollback)
        int newCap = (tier1Capacity_ < INT_MAX / 2)
            ? roundUpPow2(std::max(ringTotal, tier1Capacity_ + newRows) + 64)
            : roundUpPow2(ringTotal + 64);
        ring_.assign(static_cast<size_t>(newCap) * newCols, Cell{});
        ringExtras_.assign(newCap, {});
        continued_.assign(newCap, false);
        promptKind_.assign(newCap, UnknownPrompt);

        for (int i = 0; i < ringTotal && i < totalDstRows; ++i) {
            std::memcpy(&ring_[static_cast<size_t>(i) * newCols],
                        &dstCells[static_cast<size_t>(i) * newCols],
                        newCols * sizeof(Cell));
            ringExtras_[i] = std::move(dstExtras[i]);
            continued_[i] = dstContinued[i];
        }

        ringCapacity_ = newCap;
        ringHead_ = ringTotal & (newCap - 1);
        historyCount_ = newHistoryCount;
        cols_ = newCols;
        screenHeight_ = newRows;

    } else if (newRows != screenHeight_) {
        // Height-only change: no reflow needed
        if (newRows < screenHeight_) {
            int delta = screenHeight_ - newRows;
            // Count trailing blank rows at the bottom — discard them instead of
            // pushing them to history (they're padding, not content).
            // A row with the cursor is never blank (cursor anchors the line).
            int cursorScreenRow = cursor ? (cursor->srcY - historySize()) : -1;
            int blanksAtBottom = 0;
            for (int r = screenHeight_ - 1; r >= 0 && blanksAtBottom < delta; --r) {
                if (r == cursorScreenRow) break; // cursor row is never discardable
                int phys = screenRowToPhysical(r);
                const Cell* rp = &ring_[static_cast<size_t>(phys) * cols_];
                bool blank = true;
                for (int c = 0; c < cols_ && blank; ++c) {
                    if (rp[c].wc != 0) blank = false;
                }
                if (!blank) break;
                blanksAtBottom++;
            }
            // Discard blank rows by pulling ringHead_ back.
            ringHead_ = (ringHead_ - blanksAtBottom + ringCapacity_) & ringMask();
            // Push the remaining rows to history.
            int pushToHistory = delta - blanksAtBottom;
            historyCount_ += pushToHistory;
            while (historyCount_ > tier1Capacity_) {
                evictToArchive();
            }
        } else {
            int delta = newRows - screenHeight_;
            // Don't pull history back to screen — that causes old content (e.g. prompts)
            // to briefly reappear. Add blank rows at the bottom only.
            // Ensure ring capacity before advancing ringHead_.
            int needed = historyCount_ + newRows;
            if (needed > ringCapacity_) {
                int newCap = roundUpPow2(needed + 64);
                std::vector<Cell> newRing(static_cast<size_t>(newCap) * cols_);
                std::vector<std::unordered_map<int, CellExtra>> newExtras(newCap);
                std::vector<bool> newCont(newCap, false);
                std::vector<PromptKind> newPK(newCap, UnknownPrompt);
                int total = historyCount_ + screenHeight_;
                for (int i = 0; i < total; ++i) {
                    int oldPhys = (ringHead_ - total + i) & ringMask();
                    std::memcpy(&newRing[static_cast<size_t>(i) * cols_],
                                &ring_[static_cast<size_t>(oldPhys) * cols_], cols_ * sizeof(Cell));
                    newExtras[i] = std::move(ringExtras_[oldPhys]);
                    newCont[i] = continued_[oldPhys];
                    newPK[i] = promptKind_[oldPhys];
                }
                ring_ = std::move(newRing);
                ringExtras_ = std::move(newExtras);
                continued_ = std::move(newCont);
                promptKind_ = std::move(newPK);
                ringCapacity_ = newCap;
                ringHead_ = total & (newCap - 1);
            }
            for (int i = 0; i < delta; ++i) {
                clearPhysicalRow(ringHead_);
                ringHead_ = (ringHead_ + 1) & ringMask();
            }
        }
        screenHeight_ = newRows;

        // Ensure ring capacity (may be a no-op for grow, which already expanded above)
        int needed = historyCount_ + screenHeight_;
        if (needed > ringCapacity_) {
            int newCap = roundUpPow2(needed + 64);
            std::vector<Cell> newRing(static_cast<size_t>(newCap) * cols_);
            std::vector<std::unordered_map<int, CellExtra>> newExtras(newCap);
            std::vector<bool> newCont(newCap, false);
            std::vector<PromptKind> newPK(newCap, UnknownPrompt);
            int total = historyCount_ + screenHeight_;
            for (int i = 0; i < total; ++i) {
                int oldPhys = (ringHead_ - total + i) & ringMask();
                std::memcpy(&newRing[static_cast<size_t>(i) * cols_],
                            &ring_[static_cast<size_t>(oldPhys) * cols_], cols_ * sizeof(Cell));
                newExtras[i] = std::move(ringExtras_[oldPhys]);
                newCont[i] = continued_[oldPhys];
                newPK[i] = promptKind_[oldPhys];
            }
            ring_ = std::move(newRing);
            ringExtras_ = std::move(newExtras);
            continued_ = std::move(newCont);
            promptKind_ = std::move(newPK);
            ringCapacity_ = newCap;
            ringHead_ = total & (newCap - 1);
        }
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
