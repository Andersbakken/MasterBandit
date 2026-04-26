#include "TerminalEmulator.h"
#include "Utf8.h"
#include <algorithm>
#include <cstdio>

void TerminalEmulator::focusEvent(bool focused)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (mState->focusReporting) {
        writeToOutput(focused ? "\x1b[I" : "\x1b[O", 3);
    }
}

void TerminalEmulator::notifyColorPreference(bool isDark)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (mState->colorPreferenceReporting) {
        char response[16];
        int rlen = snprintf(response, sizeof(response), "\x1b[?997;%dn", isDark ? 1 : 2);
        writeToOutput(response, rlen);
    }
}

void TerminalEmulator::sendMouseEvent(int button, bool press, bool motion, int cx, int cy, uint32_t modifiers)
{
    sendMouseEventPixel(button, press, motion, cx, cy, -1, -1, modifiers);
}

void TerminalEmulator::sendMouseEventPixel(int button, bool press, bool motion, int cx, int cy, int px, int py, uint32_t modifiers)
{
    // Encode modifier bits into button code
    int cb = button;
    if (motion) cb += 32;
    if (modifiers & ShiftModifier) cb += 4;
    if (modifiers & AltModifier) cb += 8;
    if (modifiers & CtrlModifier) cb += 16;

    if (mState->mouseMode1016 && px >= 0 && py >= 0) {
        // SGR-Pixel format: same as SGR but with pixel coordinates
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, px + 1, py + 1, press ? 'M' : 'm');
        writeToOutput(buf, n);
    } else if (mState->mouseMode1006) {
        // SGR format: \x1b[<Cb;Cx;CyM (press) or m (release)
        // 1-based cell coordinates
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, cx + 1, cy + 1, press ? 'M' : 'm');
        writeToOutput(buf, n);
    } else {
        // Legacy format: \x1b[Mcb cx cy (all + 32)
        int x = cx + 1;
        int y = cy + 1;
        if (x > 223 || y > 223) return; // can't encode
        char buf[6];
        buf[0] = '\x1b';
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = static_cast<char>(cb + 32);
        buf[4] = static_cast<char>(x + 32);
        buf[5] = static_cast<char>(y + 32);
        writeToOutput(buf, 6);
    }
}

static int buttonToCode(Button button)
{
    switch (button) {
    case LeftButton: return 0;
    case MidButton: return 1;
    case RightButton: return 2;
    case WheelUp: return 64;
    case WheelDown: return 65;
    default: return 0;
    }
}

void TerminalEmulator::mousePressEvent(const MouseEvent *ev)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Shift overrides mouse reporting → always select
    bool forceSelect = (ev->modifiers & ShiftModifier) != 0;

    if (!forceSelect && mouseReportingActive()) {
        int btn = buttonToCode(ev->button);
        bool isWheel = (ev->button == WheelUp || ev->button == WheelDown);
        if (!isWheel) {
            mMouseButtonDown = btn;
            mLastMouseX = ev->x;
            mLastMouseY = ev->y;
        }
        sendMouseEventPixel(btn, true, false, ev->x, ev->y, ev->pixelX, ev->pixelY, ev->modifiers);
        return;
    }

    // Arm pending selection — actual selection starts only when the mouse moves
    clearSelection();
    mPendingSelection    = true;
    mPendingSelCol       = ev->x;
    mPendingSelAbsRow    = mDocument.historySize() - viewportOffset() + ev->y;
}

void TerminalEmulator::mouseReleaseEvent(const MouseEvent *ev)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Button released without moving — discard pending selection
    mPendingSelection = false;

    if (mSelection.active) {
        finalizeSelection();
        std::string text = selectedText();
        if (!text.empty()) {
            if (mCallbacks.copyToClipboard) mCallbacks.copyToClipboard(text);
        }
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
        return;
    }

    if (mouseReportingActive() && mMouseButtonDown >= 0) {
        int btn = buttonToCode(ev->button);
        if (mState->mouseMode1006 || mState->mouseMode1016) {
            sendMouseEventPixel(btn, false, false, ev->x, ev->y, ev->pixelX, ev->pixelY, ev->modifiers);
        } else {
            // Legacy: release is button code 3
            sendMouseEventPixel(3, false, false, ev->x, ev->y, ev->pixelX, ev->pixelY, ev->modifiers);
        }
        mMouseButtonDown = -1;
        mLastMouseX = -1;
        mLastMouseY = -1;
    }
}

void TerminalEmulator::mouseMoveEvent(const MouseEvent *ev)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // First move after press — activate the pending selection
    if (mPendingSelection) {
        mPendingSelection = false;
        startSelection(mPendingSelCol, mPendingSelAbsRow);
    }

    if (mSelection.active) {
        int col = std::max(0, std::min(ev->x, mWidth - 1));
        int row = std::max(0, std::min(ev->y, mHeight - 1));
        int absRow = mDocument.historySize() - viewportOffset() + row;
        updateSelection(col, absRow);
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
        return;
    }

    if (ev->x == mLastMouseX && ev->y == mLastMouseY) return;

    if (mState->mouseMode1003) {
        int btn = (mMouseButtonDown >= 0) ? mMouseButtonDown : 3; // 3 = no button in motion
        sendMouseEventPixel(btn, true, true, ev->x, ev->y, ev->pixelX, ev->pixelY, ev->modifiers);
        mLastMouseX = ev->x;
        mLastMouseY = ev->y;
    } else if (mState->mouseMode1002 && mMouseButtonDown >= 0) {
        sendMouseEventPixel(mMouseButtonDown, true, true, ev->x, ev->y, ev->pixelX, ev->pixelY, ev->modifiers);
        mLastMouseX = ev->x;
        mLastMouseY = ev->y;
    }
}

// --- Word boundary detection ---

static bool isWordChar(char32_t ch)
{
    if (ch == 0) return false;
    // Alphanumeric, underscore, and common path/URL characters
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
        ch == '/' || ch == '~' || ch == ':')
        return true;
    // Non-ASCII: treat as word character (CJK, etc.)
    if (ch > 127) return true;
    return false;
}

// Get the cell at a given absolute row and column.
// Returns null char if out of bounds.
static char32_t cellAt(const TerminalEmulator& te, int col, int absRow)
{
    int histSize = te.document().historySize();
    const Cell* row;
    if (absRow < histSize) {
        row = te.document().historyRow(absRow);
    } else {
        int gridRow = absRow - histSize;
        if (gridRow < 0 || gridRow >= te.grid().rows()) return 0;
        row = te.grid().row(gridRow);
    }
    if (!row || col < 0 || col >= te.width()) return 0;
    return row[col].wc;
}

// Selection implementation
//
// Storage is `(lineId, cellOffsetWithinLine)` per anchor — see
// TerminalEmulator.h Selection for details. Same model as iTerm2's
// `LineBufferPosition.absolutePosition`: a logical text position that's
// invariant under column reflow.
namespace {
inline int cellOffsetWithinLine(const Document& doc, uint64_t id, int absRow,
                                int col, int width)
{
    int firstAbs = doc.firstAbsOfLine(id);
    int rowOff = (firstAbs < 0) ? 0 : (absRow - firstAbs);
    return rowOff * width + col;
}
} // namespace

void TerminalEmulator::startSelection(int col, int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    uint64_t id = mDocument.lineIdForAbs(absRow);
    int off = cellOffsetWithinLine(mDocument, id, absRow, col, mWidth);
    mSelection.startLineId = id; mSelection.startCellOffset = off;
    mSelection.endLineId   = id; mSelection.endCellOffset   = off;
    mSelection.active = true;
    mSelection.valid = false;
    mSelection.mode = SelectionMode::Normal;
}

void TerminalEmulator::startWordSelection(int col, int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Scan left and right for word boundaries
    int left = col, right = col;
    char32_t ch = cellAt(*this, col, absRow);
    bool isWord = isWordChar(ch);

    while (left > 0) {
        char32_t c = cellAt(*this, left - 1, absRow);
        if (isWordChar(c) != isWord) break;
        left--;
    }
    while (right < mWidth - 1) {
        char32_t c = cellAt(*this, right + 1, absRow);
        if (isWordChar(c) != isWord) break;
        right++;
    }

    uint64_t id = mDocument.lineIdForAbs(absRow);
    mSelection.startLineId = id;
    mSelection.startCellOffset = cellOffsetWithinLine(mDocument, id, absRow, left,  mWidth);
    mSelection.endLineId   = id;
    mSelection.endCellOffset   = cellOffsetWithinLine(mDocument, id, absRow, right, mWidth);
    mSelection.active = true;
    mSelection.valid = false;
    mSelection.mode = SelectionMode::Word;

    finalizeSelection();
    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard)
        mCallbacks.copyToClipboard(text);
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::startLineSelection(int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    uint64_t id = mDocument.lineIdForAbs(absRow);
    // Span the entire logical line, including any wrapped continuation
    // rows. Use lastAbsOfLine to get the final visual row of this line at
    // the current width; the end cellOffset is the last cell of that row.
    int firstAbs = mDocument.firstAbsOfLine(id);
    int lastAbs  = mDocument.lastAbsOfLine(id);
    int rowSpan  = (firstAbs < 0 || lastAbs < 0) ? 0 : (lastAbs - firstAbs);
    mSelection.startLineId = id; mSelection.startCellOffset = 0;
    mSelection.endLineId   = id; mSelection.endCellOffset   = rowSpan * mWidth + (mWidth - 1);
    mSelection.active = true;
    mSelection.valid = false;
    mSelection.mode = SelectionMode::Line;

    finalizeSelection();
    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard)
        mCallbacks.copyToClipboard(text);
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::extendSelection(int col, int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (!mSelection.valid && !mSelection.active) {
        startSelection(col, absRow);
        return;
    }

    auto resOpt = resolveSelection();
    if (!resOpt) {
        startSelection(col, absRow);
        return;
    }
    int distToStart = std::abs(absRow - resOpt->startAbsRow) * mWidth +
                      std::abs(col - resOpt->startCol);
    int distToEnd   = std::abs(absRow - resOpt->endAbsRow)   * mWidth +
                      std::abs(col - resOpt->endCol);

    uint64_t newId = mDocument.lineIdForAbs(absRow);
    int newOff = cellOffsetWithinLine(mDocument, newId, absRow, col, mWidth);
    if (distToStart < distToEnd) {
        mSelection.startLineId = newId; mSelection.startCellOffset = newOff;
    } else {
        mSelection.endLineId   = newId; mSelection.endCellOffset   = newOff;
    }
    mSelection.active = true;
    mSelection.valid = false;
    mSelection.mode = SelectionMode::Normal;

    finalizeSelection();
    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard)
        mCallbacks.copyToClipboard(text);
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::startRectangleSelection(int col, int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    uint64_t id = mDocument.lineIdForAbs(absRow);
    int off = cellOffsetWithinLine(mDocument, id, absRow, col, mWidth);
    mSelection.startLineId = id; mSelection.startCellOffset = off;
    mSelection.endLineId   = id; mSelection.endCellOffset   = off;
    mSelection.active = true;
    mSelection.valid = false;
    mSelection.mode = SelectionMode::Rectangle;
}

void TerminalEmulator::updateSelection(int col, int absRow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    uint64_t id = mDocument.lineIdForAbs(absRow);
    mSelection.endLineId     = id;
    mSelection.endCellOffset = cellOffsetWithinLine(mDocument, id, absRow, col, mWidth);
}

void TerminalEmulator::finalizeSelection()
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    mSelection.active = false;
    mSelection.valid = true;
}

void TerminalEmulator::clearSelection()
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    bool had = mSelection.active || mSelection.valid;
    mSelection.active = false;
    mSelection.valid = false;
    if (had && mCallbacks.event)
        mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

std::optional<TerminalEmulator::ResolvedSelection>
TerminalEmulator::resolveSelection() const
{
    if (!mSelection.active && !mSelection.valid) return std::nullopt;
    int startFirst = mDocument.firstAbsOfLine(mSelection.startLineId);
    int endFirst   = mDocument.firstAbsOfLine(mSelection.endLineId);
    if (startFirst < 0 || endFirst < 0) return std::nullopt;
    // Convert cell offset back to current visual coordinates. Reflow
    // re-wrapped the logical line into rows of the current `mWidth`, so
    // cell N of the line is at row `firstAbs + N/mWidth`, col `N%mWidth`.
    // Clamp to the line's last visible row + col (a wider new layout may
    // mean the offset's row falls past the line's actual extent).
    int w = std::max(1, mWidth);
    auto resolve = [&](uint64_t id, int firstAbs, int cellOff,
                       int& outRow, int& outCol) {
        int row = firstAbs + cellOff / w;
        int col = cellOff % w;
        int lastAbs = mDocument.lastAbsOfLine(id);
        if (lastAbs >= 0 && row > lastAbs) {
            row = lastAbs;
            col = w - 1;
        }
        outRow = row;
        outCol = col;
    };
    ResolvedSelection r;
    resolve(mSelection.startLineId, startFirst, mSelection.startCellOffset,
            r.startAbsRow, r.startCol);
    resolve(mSelection.endLineId,   endFirst,   mSelection.endCellOffset,
            r.endAbsRow, r.endCol);
    r.active = mSelection.active;
    r.valid  = mSelection.valid;
    r.mode   = mSelection.mode;
    return r;
}

bool TerminalEmulator::hasSelection() const
{
    return resolveSelection().has_value();
}

bool TerminalEmulator::isCellSelected(int col, int absRow) const
{
    auto resOpt = resolveSelection();
    if (!resOpt) return false;
    const auto& res = *resOpt;
    int r0 = res.startAbsRow, c0 = res.startCol;
    int r1 = res.endAbsRow,   c1 = res.endCol;

    if (res.mode == SelectionMode::Rectangle) {
        // Rectangle: column range is independent of row
        int minR = std::min(r0, r1), maxR = std::max(r0, r1);
        int minC = std::min(c0, c1), maxC = std::max(c0, c1);
        return absRow >= minR && absRow <= maxR && col >= minC && col <= maxC;
    }

    // Normal/word/line: linear selection
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }

    if (absRow < r0 || absRow > r1) return false;
    if (absRow == r0 && absRow == r1) return col >= c0 && col <= c1;
    if (absRow == r0) return col >= c0;
    if (absRow == r1) return col <= c1;
    return true;
}

std::string TerminalEmulator::selectedText() const
{
    auto resOpt = resolveSelection();
    if (!resOpt) return {};
    const auto& res = *resOpt;
    int r0 = res.startAbsRow, c0 = res.startCol;
    int r1 = res.endAbsRow,   c1 = res.endCol;

    if (res.mode == SelectionMode::Rectangle) {
        // Rectangle: same column range on every row
        int minR = std::min(r0, r1), maxR = std::max(r0, r1);
        int minC = std::min(c0, c1), maxC = std::max(c0, c1);
        r0 = minR; r1 = maxR; c0 = minC; c1 = maxC;
    } else {
        if (r0 > r1 || (r0 == r1 && c0 > c1)) {
            std::swap(r0, r1);
            std::swap(c0, c1);
        }
    }

    int histSize = mDocument.historySize();
    std::string result;

    for (int absRow = r0; absRow <= r1; ++absRow) {
        const Cell* row;
        const std::unordered_map<int, CellExtra>* extras = nullptr;
        if (absRow < histSize) {
            row = mDocument.historyRow(absRow);
            extras = mDocument.historyExtras(absRow);
        } else {
            int gridRow = absRow - histSize;
            if (gridRow < 0 || gridRow >= grid().rows()) continue;
            row = grid().row(gridRow);
            extras = mDocument.viewportExtras(gridRow, 0);
        }
        if (!row) continue;

        int colStart, colEnd;
        if (res.mode == SelectionMode::Rectangle) {
            colStart = c0;
            colEnd = c1;
        } else {
            colStart = (absRow == r0) ? c0 : 0;
            colEnd = (absRow == r1) ? c1 : (mWidth - 1);
        }
        colStart = std::max(0, std::min(colStart, mWidth - 1));
        colEnd = std::max(0, std::min(colEnd, mWidth - 1));

        // Collect text, trimming trailing spaces
        std::string line;
        int lastNonSpace = colStart - 1;
        for (int col = colStart; col <= colEnd; ++col) {
            const Cell& cell = row[col];
            if (cell.attrs.wideSpacer()) continue;
            if (cell.wc == 0 || cell.wc == ' ') {
                line += ' ';
            } else {
                line += utf8::fromCodepoint(cell.wc);
                if (extras) {
                    auto it = extras->find(col);
                    if (it != extras->end()) {
                        for (char32_t cp : it->second.combiningCps)
                            line += utf8::fromCodepoint(cp);
                    }
                }
                lastNonSpace = static_cast<int>(line.size()) - 1;
            }
        }
        // Trim trailing spaces
        if (lastNonSpace >= 0) {
            // Find the byte position after the last non-space content
            line.resize(lastNonSpace + 1);
        } else {
            line.clear();
        }

        if (absRow > r0) {
            // Don't insert a newline if the previous row was soft-wrapped
            int prevAbsRow = absRow - 1;
            bool prevContinued = false;
            if (prevAbsRow < histSize)
                prevContinued = mDocument.isHistoryRowContinued(prevAbsRow);
            else {
                int prevGridRow = prevAbsRow - histSize;
                if (prevGridRow >= 0 && prevGridRow < mDocument.rows())
                    prevContinued = mDocument.isRowContinued(prevGridRow);
            }
            if (!prevContinued)
                result += '\n';
        }
        result += line;
    }

    return result;
}
