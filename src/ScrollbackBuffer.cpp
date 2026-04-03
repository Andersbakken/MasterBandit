#include "ScrollbackBuffer.h"
#include <algorithm>
#include <cstring>

// --- Helpers for UTF-8 encode/decode ---

static int encodeUtf8(char32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    } else if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
}

static char32_t decodeUtf8(const char* s, int len, int& bytesConsumed) {
    auto u = [](char c) -> uint8_t { return static_cast<uint8_t>(c); };
    if (len <= 0) { bytesConsumed = 0; return 0; }
    uint8_t b0 = u(s[0]);
    if (b0 < 0x80) {
        bytesConsumed = 1;
        return b0;
    } else if ((b0 & 0xE0) == 0xC0 && len >= 2) {
        bytesConsumed = 2;
        return ((b0 & 0x1F) << 6) | (u(s[1]) & 0x3F);
    } else if ((b0 & 0xF0) == 0xE0 && len >= 3) {
        bytesConsumed = 3;
        return ((b0 & 0x0F) << 12) | ((u(s[1]) & 0x3F) << 6) | (u(s[2]) & 0x3F);
    } else if ((b0 & 0xF8) == 0xF0 && len >= 4) {
        bytesConsumed = 4;
        return ((b0 & 0x07) << 18) | ((u(s[1]) & 0x3F) << 12) |
               ((u(s[2]) & 0x3F) << 6) | (u(s[3]) & 0x3F);
    }
    bytesConsumed = 1;
    return 0xFFFD; // replacement char
}

// --- SGR serialization helpers ---

// Emit SGR escape to switch from old attrs to new attrs.
// Returns number of bytes appended.
static void emitSGR(std::string& out, const CellAttrs& oldA, const CellAttrs& newA) {
    // Check if reset is simpler (new has fewer set attributes)
    bool needReset = false;
    // If any attribute was on in old but off in new, reset is often simpler
    if ((oldA.bold() && !newA.bold()) ||
        (oldA.italic() && !newA.italic()) ||
        (oldA.underline() && !newA.underline()) ||
        (oldA.strikethrough() && !newA.strikethrough()) ||
        (oldA.blink() && !newA.blink()) ||
        (oldA.inverse() && !newA.inverse()) ||
        (oldA.dim() && !newA.dim()) ||
        (oldA.invisible() && !newA.invisible())) {
        needReset = true;
    }

    std::string params;
    auto appendParam = [&](const char* p) {
        if (!params.empty()) params += ';';
        params += p;
    };
    auto appendInt = [&](int v) {
        if (!params.empty()) params += ';';
        params += std::to_string(v);
    };

    CellAttrs base{}; // default attrs for comparison after reset
    const CellAttrs& cmp = needReset ? base : oldA;

    if (needReset) {
        appendParam("0");
    }

    if (newA.bold() && !cmp.bold()) appendParam("1");
    if (newA.dim() && !cmp.dim()) appendParam("2");
    if (newA.italic() && !cmp.italic()) appendParam("3");
    if (newA.underline() && !cmp.underline()) appendParam("4");
    if (newA.blink() && !cmp.blink()) appendParam("5");
    if (newA.inverse() && !cmp.inverse()) appendParam("7");
    if (newA.invisible() && !cmp.invisible()) appendParam("8");
    if (newA.strikethrough() && !cmp.strikethrough()) appendParam("9");

    // Foreground color
    if (newA.fgMode() != cmp.fgMode() ||
        (newA.fgMode() != CellAttrs::Default &&
         (newA.fgR() != cmp.fgR() || newA.fgG() != cmp.fgG() || newA.fgB() != cmp.fgB()))) {
        if (newA.fgMode() == CellAttrs::Default) {
            appendParam("39");
        } else if (newA.fgMode() == CellAttrs::RGB) {
            appendParam("38");
            appendParam("2");
            appendInt(newA.fgR());
            appendInt(newA.fgG());
            appendInt(newA.fgB());
        } else {
            // Indexed - store as 38;5;N
            // We need to reconstruct the index from RGB - but since we don't store
            // the original index, use RGB mode universally
            appendParam("38");
            appendParam("2");
            appendInt(newA.fgR());
            appendInt(newA.fgG());
            appendInt(newA.fgB());
        }
    }

    // Background color
    if (newA.bgMode() != cmp.bgMode() ||
        (newA.bgMode() != CellAttrs::Default &&
         (newA.bgR() != cmp.bgR() || newA.bgG() != cmp.bgG() || newA.bgB() != cmp.bgB()))) {
        if (newA.bgMode() == CellAttrs::Default) {
            appendParam("49");
        } else if (newA.bgMode() == CellAttrs::RGB) {
            appendParam("48");
            appendParam("2");
            appendInt(newA.bgR());
            appendInt(newA.bgG());
            appendInt(newA.bgB());
        } else {
            appendParam("48");
            appendParam("2");
            appendInt(newA.bgR());
            appendInt(newA.bgG());
            appendInt(newA.bgB());
        }
    }

    // Wide attribute - encode as private param 100
    if (newA.wide() != cmp.wide()) {
        if (newA.wide()) appendParam("100");
    }
    if (newA.wideSpacer() != cmp.wideSpacer()) {
        if (newA.wideSpacer()) appendParam("101");
    }

    if (!params.empty()) {
        out += '\x1b';
        out += '[';
        out += params;
        out += 'm';
    }
}

// --- Parse SGR params back into CellAttrs ---

static void parseSGRParams(const char* start, int len, CellAttrs& attrs) {
    // Parse semicolon-delimited integers
    std::vector<int> params;
    int val = 0;
    bool hasVal = false;
    for (int i = 0; i < len; ++i) {
        if (start[i] >= '0' && start[i] <= '9') {
            val = val * 10 + (start[i] - '0');
            hasVal = true;
        } else if (start[i] == ';') {
            params.push_back(hasVal ? val : 0);
            val = 0;
            hasVal = false;
        }
    }
    if (hasVal) params.push_back(val);

    for (size_t i = 0; i < params.size(); ++i) {
        switch (params[i]) {
        case 0:
            attrs.reset();
            break;
        case 1: attrs.setBold(true); break;
        case 2: attrs.setDim(true); break;
        case 3: attrs.setItalic(true); break;
        case 4: attrs.setUnderline(true); break;
        case 5: attrs.setBlink(true); break;
        case 7: attrs.setInverse(true); break;
        case 8: attrs.setInvisible(true); break;
        case 9: attrs.setStrikethrough(true); break;
        case 39:
            attrs.setFgMode(CellAttrs::Default);
            break;
        case 49:
            attrs.setBgMode(CellAttrs::Default);
            break;
        case 38:
            // 38;2;R;G;B
            if (i + 4 < params.size() && params[i + 1] == 2) {
                attrs.setFgMode(CellAttrs::RGB);
                attrs.setFg(static_cast<uint8_t>(params[i + 2]),
                            static_cast<uint8_t>(params[i + 3]),
                            static_cast<uint8_t>(params[i + 4]));
                i += 4;
            }
            break;
        case 48:
            // 48;2;R;G;B
            if (i + 4 < params.size() && params[i + 1] == 2) {
                attrs.setBgMode(CellAttrs::RGB);
                attrs.setBg(static_cast<uint8_t>(params[i + 2]),
                            static_cast<uint8_t>(params[i + 3]),
                            static_cast<uint8_t>(params[i + 4]));
                i += 4;
            }
            break;
        case 100:
            attrs.setWide(true);
            break;
        case 101:
            attrs.setWideSpacer(true);
            break;
        default:
            break;
        }
    }
}

// =========================================================================
// ScrollbackBuffer implementation
// =========================================================================

int ScrollbackBuffer::roundUpPow2(int v) {
    if (v <= 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

ScrollbackBuffer::ScrollbackBuffer(int cols, int tier1Capacity, int maxArchiveRows)
    : cols_(cols)
    , tier1Capacity_(roundUpPow2(tier1Capacity))
    , maxArchiveRows_(maxArchiveRows)
{
    if (cols_ > 0) {
        ring_.resize(static_cast<size_t>(tier1Capacity_) * cols_);
        ringExtras_.resize(tier1Capacity_);
    }
}

int ScrollbackBuffer::historySize() const {
    return static_cast<int>(archive_.size()) + tier1Size_;
}

int ScrollbackBuffer::tier1ToPhysical(int tier1Idx) const {
    // oldest tier1 row is at (ringHead_ - tier1Size_) mod capacity
    return (ringHead_ - tier1Size_ + tier1Idx) & ringMask();
}

const Cell* ScrollbackBuffer::historyRow(int idx) const {
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) {
        parseArchivedRow(archive_[idx]);
        return parseBuffer_.data();
    } else {
        int physSlot = tier1ToPhysical(idx - archiveSize);
        return &ring_[static_cast<size_t>(physSlot) * cols_];
    }
}

void ScrollbackBuffer::pushHistory(const Cell* row, int numCols,
                                    std::unordered_map<int, CellExtra>&& extras) {
    if (cols_ <= 0) return;

    if (tier1Size_ == tier1Capacity_) {
        // Evict oldest Tier 1 row → Tier 2
        int oldest = tier1ToPhysical(0);
        archive_.push_back({serializeRow(&ring_[static_cast<size_t>(oldest) * cols_], cols_)});
        ringExtras_[oldest].clear();
        if (static_cast<int>(archive_.size()) > maxArchiveRows_) {
            archive_.pop_front();
        }
        tier1Size_--;
    }

    // Write into ring at ringHead_
    Cell* dst = &ring_[static_cast<size_t>(ringHead_) * cols_];
    int copyCount = std::min(numCols, cols_);
    std::memcpy(dst, row, static_cast<size_t>(copyCount) * sizeof(Cell));
    if (copyCount < cols_) {
        std::memset(dst + copyCount, 0, static_cast<size_t>(cols_ - copyCount) * sizeof(Cell));
    }
    ringExtras_[ringHead_] = std::move(extras);
    ringHead_ = (ringHead_ + 1) & ringMask();
    tier1Size_++;
}

const std::unordered_map<int, CellExtra>* ScrollbackBuffer::historyExtras(int idx) const {
    int archiveSize = static_cast<int>(archive_.size());
    if (idx < archiveSize) {
        return nullptr; // tier 2 archived rows don't preserve extras
    }
    int tier1Idx = idx - archiveSize;
    if (tier1Idx < 0 || tier1Idx >= tier1Size_) return nullptr;
    int physical = tier1ToPhysical(tier1Idx);
    const auto& m = ringExtras_[physical];
    return m.empty() ? nullptr : &m;
}

void ScrollbackBuffer::resize(int newCols) {
    if (newCols == cols_ && !ring_.empty()) return;
    if (newCols <= 0) {
        cols_ = 0;
        ring_.clear();
        ringExtras_.clear();
        tier1Size_ = 0;
        ringHead_ = 0;
        archive_.clear();
        return;
    }

    // Rebuild ring preserving row order
    std::vector<Cell> newRing(static_cast<size_t>(tier1Capacity_) * newCols);
    int minCols = std::min(cols_, newCols);
    for (int i = 0; i < tier1Size_; ++i) {
        int oldPhys = tier1ToPhysical(i);
        const Cell* src = &ring_[static_cast<size_t>(oldPhys) * cols_];
        Cell* dst = &newRing[static_cast<size_t>(i) * newCols];
        if (minCols > 0) {
            std::memcpy(dst, src, static_cast<size_t>(minCols) * sizeof(Cell));
        }
        if (newCols > minCols) {
            std::memset(dst + minCols, 0, static_cast<size_t>(newCols - minCols) * sizeof(Cell));
        }
    }
    // Rebuild extras ring preserving order
    std::vector<std::unordered_map<int, CellExtra>> newExtras(tier1Capacity_);
    for (int i = 0; i < tier1Size_; ++i) {
        int oldPhys = tier1ToPhysical(i);
        newExtras[i] = std::move(ringExtras_[oldPhys]);
    }

    ring_ = std::move(newRing);
    ringExtras_ = std::move(newExtras);
    ringHead_ = tier1Size_ & ringMask();
    cols_ = newCols;

    // Archive rows are not resized; parseBuffer_ will adapt on read
    parseBuffer_.clear();
}

void ScrollbackBuffer::clearHistory() {
    archive_.clear();
    tier1Size_ = 0;
    ringHead_ = 0;
    parseBuffer_.clear();
    for (auto& m : ringExtras_) m.clear();
}

std::string ScrollbackBuffer::serializeRow(const Cell* cells, int cols) {
    std::string out;
    out.reserve(static_cast<size_t>(cols)); // rough estimate

    // Find last non-empty cell to trim trailing empties
    int lastNonEmpty = cols - 1;
    while (lastNonEmpty >= 0 && cells[lastNonEmpty].wc == 0 &&
           std::memcmp(cells[lastNonEmpty].attrs.data, CellAttrs{}.data, sizeof(CellAttrs::data)) == 0) {
        --lastNonEmpty;
    }

    CellAttrs currentAttrs{};
    char utf8Buf[4];

    for (int i = 0; i <= lastNonEmpty; ++i) {
        const Cell& c = cells[i];
        // Emit SGR if attrs changed
        if (std::memcmp(c.attrs.data, currentAttrs.data, sizeof(CellAttrs::data)) != 0) {
            emitSGR(out, currentAttrs, c.attrs);
            currentAttrs = c.attrs;
        }
        // Emit codepoint as UTF-8
        if (c.wc == 0) {
            out += ' '; // empty cell → space
        } else {
            int n = encodeUtf8(c.wc, utf8Buf);
            out.append(utf8Buf, n);
        }
    }

    return out;
}

void ScrollbackBuffer::parseArchivedRow(const ArchivedRow& row) const {
    parseBuffer_.resize(cols_);
    std::memset(parseBuffer_.data(), 0, static_cast<size_t>(cols_) * sizeof(Cell));

    CellAttrs currentAttrs{};
    int col = 0;
    const char* s = row.data.data();
    int len = static_cast<int>(row.data.size());
    int pos = 0;

    while (pos < len && col < cols_) {
        if (s[pos] == '\x1b' && pos + 1 < len && s[pos + 1] == '[') {
            // Parse SGR sequence: ESC [ ... m
            pos += 2; // skip ESC [
            int start = pos;
            while (pos < len && s[pos] != 'm') ++pos;
            if (pos < len) {
                parseSGRParams(s + start, pos - start, currentAttrs);
                ++pos; // skip 'm'
            }
        } else {
            // Decode UTF-8 codepoint
            int consumed = 0;
            char32_t cp = decodeUtf8(s + pos, len - pos, consumed);
            parseBuffer_[col].wc = cp;
            parseBuffer_[col].attrs = currentAttrs;
            ++col;
            pos += consumed;
        }
    }
}
