#include "TerminalEmulator.h"
#include "Utf8.h"
#include <algorithm>
#include <cstdio>

void TerminalEmulator::focusEvent(bool focused)
{
    if (mFocusReporting) {
        writeToOutput(focused ? "\x1b[I" : "\x1b[O", 3);
    }
}

void TerminalEmulator::notifyColorPreference(bool isDark)
{
    if (mColorPreferenceReporting) {
        char response[16];
        int rlen = snprintf(response, sizeof(response), "\x1b[?997;%dn", isDark ? 1 : 2);
        writeToOutput(response, rlen);
    }
}

void TerminalEmulator::sendMouseEvent(int button, bool press, bool motion, int cx, int cy, unsigned int modifiers)
{
    // Encode modifier bits into button code
    int cb = button;
    if (motion) cb += 32;
    if (modifiers & ShiftModifier) cb += 4;
    if (modifiers & AltModifier) cb += 8;
    if (modifiers & CtrlModifier) cb += 16;

    // 1-based coordinates
    int x = cx + 1;
    int y = cy + 1;

    if (mMouseMode1006) {
        // SGR format: \x1b[<Cb;Cx;CyM (press) or m (release)
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", cb, x, y, press ? 'M' : 'm');
        writeToOutput(buf, n);
    } else {
        // Legacy format: \x1b[Mcb cx cy (all + 32)
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
    default: return 0;
    }
}

void TerminalEmulator::mousePressEvent(const MouseEvent *ev)
{
    // Shift overrides mouse reporting → always select
    bool forceSelect = (ev->modifiers & ShiftModifier) != 0;

    if (!forceSelect && mouseReportingActive()) {
        int btn = buttonToCode(ev->button);
        mMouseButtonDown = btn;
        mLastMouseX = ev->x;
        mLastMouseY = ev->y;
        sendMouseEvent(btn, true, false, ev->x, ev->y, ev->modifiers);
        return;
    }

    // Arm pending selection — actual selection starts only when the mouse moves
    clearSelection();
    mPendingSelection    = true;
    mPendingSelCol       = ev->x;
    mPendingSelAbsRow    = mDocument.historySize() - mViewportOffset + ev->y;
}

void TerminalEmulator::mouseReleaseEvent(const MouseEvent *ev)
{
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
        if (mMouseMode1006) {
            sendMouseEvent(btn, false, false, ev->x, ev->y, ev->modifiers);
        } else {
            // Legacy: release is button code 3
            sendMouseEvent(3, false, false, ev->x, ev->y, ev->modifiers);
        }
        mMouseButtonDown = -1;
        mLastMouseX = -1;
        mLastMouseY = -1;
    }
}

void TerminalEmulator::mouseMoveEvent(const MouseEvent *ev)
{
    // First move after press — activate the pending selection
    if (mPendingSelection) {
        mPendingSelection = false;
        startSelection(mPendingSelCol, mPendingSelAbsRow);
    }

    if (mSelection.active) {
        int col = std::max(0, std::min(ev->x, mWidth - 1));
        int row = std::max(0, std::min(ev->y, mHeight - 1));
        int absRow = mDocument.historySize() - mViewportOffset + row;
        updateSelection(col, absRow);
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
        return;
    }

    if (ev->x == mLastMouseX && ev->y == mLastMouseY) return;

    if (mMouseMode1003) {
        int btn = (mMouseButtonDown >= 0) ? mMouseButtonDown : 3; // 3 = no button in motion
        sendMouseEvent(btn, true, true, ev->x, ev->y, ev->modifiers);
        mLastMouseX = ev->x;
        mLastMouseY = ev->y;
    } else if (mMouseMode1002 && mMouseButtonDown >= 0) {
        sendMouseEvent(mMouseButtonDown, true, true, ev->x, ev->y, ev->modifiers);
        mLastMouseX = ev->x;
        mLastMouseY = ev->y;
    }
}

// Selection implementation
void TerminalEmulator::startSelection(int col, int absRow)
{
    mSelection.startCol = col;
    mSelection.startAbsRow = absRow;
    mSelection.endCol = col;
    mSelection.endAbsRow = absRow;
    mSelection.active = true;
    mSelection.valid = false;
}

void TerminalEmulator::updateSelection(int col, int absRow)
{
    mSelection.endCol = col;
    mSelection.endAbsRow = absRow;
}

void TerminalEmulator::finalizeSelection()
{
    mSelection.active = false;
    mSelection.valid = true;
}

void TerminalEmulator::clearSelection()
{
    mSelection.active = false;
    mSelection.valid = false;
}

bool TerminalEmulator::isCellSelected(int col, int absRow) const
{
    if (!mSelection.active && !mSelection.valid) return false;

    int r0 = mSelection.startAbsRow, c0 = mSelection.startCol;
    int r1 = mSelection.endAbsRow, c1 = mSelection.endCol;

    // Normalize so (r0,c0) <= (r1,c1)
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
    if (!mSelection.active && !mSelection.valid) return {};

    int r0 = mSelection.startAbsRow, c0 = mSelection.startCol;
    int r1 = mSelection.endAbsRow, c1 = mSelection.endCol;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }

    int histSize = mDocument.historySize();
    std::string result;

    for (int absRow = r0; absRow <= r1; ++absRow) {
        const Cell* row;
        if (absRow < histSize) {
            row = mDocument.historyRow(absRow);
        } else {
            int gridRow = absRow - histSize;
            if (gridRow < 0 || gridRow >= grid().rows()) continue;
            row = grid().row(gridRow);
        }
        if (!row) continue;

        int colStart = (absRow == r0) ? c0 : 0;
        int colEnd = (absRow == r1) ? c1 : (mWidth - 1);
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

        if (absRow > r0) result += '\n';
        result += line;
    }

    return result;
}
