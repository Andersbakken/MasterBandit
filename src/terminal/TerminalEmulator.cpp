#include "TerminalEmulator.h"
#include "Config.h"
#include "Utils.h"
#include "Utf8.h"
#include "Wcwidth.h"
#include "Observability.h"
#include <spdlog/spdlog.h>

static spdlog::logger& sLog()
{
    static auto l = spdlog::get("terminal");
    return l ? *l : *spdlog::default_logger();
}

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <limits>
#include <algorithm>
#include <string_view>
#include <cmath>



std::string toPrintable(const char *bytes, int len)
{
    std::string ret;
    ret.reserve(len);
    for (int i=0; i<len; ++i) {
        switch (bytes[i]) {
        case '\0': ret += "\\0"; break;
        case '\a': ret += "\\a"; break;
        case '\b': ret += "\\b"; break;
        case '\t': ret += "\\t"; break;
        case '\n': ret += "\\n"; break;
        case '\v': ret += "\\v"; break;
        case '\f': ret += "\\f"; break;
        case '\r': ret += "\\r"; break;
        default:
            if (std::isprint(bytes[i])) {
                ret += bytes[i];
            } else {
                char buf[16];
                const int w = snprintf(buf, sizeof(buf), "\\0%o", bytes[i]);
                ret.append(buf, w);
            }
        }
    }
    return ret;
}


void TerminalEmulator::color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b) const
{
    auto it = m256PaletteOverrides.find(idx);
    if (it != m256PaletteOverrides.end()) {
        r = it->second[0]; g = it->second[1]; b = it->second[2]; return;
    }
    if (idx < 16) {
        r = m16ColorPalette[idx][0];
        g = m16ColorPalette[idx][1];
        b = m16ColorPalette[idx][2];
    } else if (idx < 232) {
        // 6x6x6 color cube
        int v = idx - 16;
        int bi = v % 6; v /= 6;
        int gi = v % 6; v /= 6;
        int ri = v;
        r = ri ? static_cast<uint8_t>(55 + ri * 40) : 0;
        g = gi ? static_cast<uint8_t>(55 + gi * 40) : 0;
        b = bi ? static_cast<uint8_t>(55 + bi * 40) : 0;
    } else {
        // Grayscale ramp
        uint8_t v = static_cast<uint8_t>(8 + (idx - 232) * 10);
        r = g = b = v;
    }
}

TerminalEmulator::TerminalEmulator(TerminalCallbacks callbacks)
    : mCallbacks(std::move(callbacks))
{
    memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
    memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
    applyColorScheme(ColorScheme{}); // initialize from config defaults
}

TerminalEmulator::~TerminalEmulator()
{
}

void TerminalEmulator::resetScrollback(int scrollbackLines)
{
    mDocument = Document(mDocument.cols(), mDocument.rows(), scrollbackLines);
}

void TerminalEmulator::applyColorScheme(const ColorScheme& cs)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    const std::string* colors[] = {
        &cs.color0, &cs.color1, &cs.color2, &cs.color3,
        &cs.color4, &cs.color5, &cs.color6, &cs.color7,
        &cs.color8, &cs.color9, &cs.color10, &cs.color11,
        &cs.color12, &cs.color13, &cs.color14, &cs.color15
    };
    for (int i = 0; i < 16; ++i) {
        color::parseHex(*colors[i], m16ColorPalette[i][0], m16ColorPalette[i][1], m16ColorPalette[i][2]);
    }
    color::parseHex(cs.foreground, mDefaultColors.fgR, mDefaultColors.fgG, mDefaultColors.fgB);
    color::parseHex(cs.background, mDefaultColors.bgR, mDefaultColors.bgG, mDefaultColors.bgB);
    color::parseHex(cs.cursor, mDefaultColors.cursorR, mDefaultColors.cursorG, mDefaultColors.cursorB);
    // Save config-loaded values for OSC 104/110/111/112 reset
    memcpy(m16PaletteDefaults, m16ColorPalette, sizeof(m16ColorPalette));
    mConfigDefaultColors = mDefaultColors;
}

void TerminalEmulator::applyCursorConfig(const CursorConfig& cc)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    CursorShape shape;
    if (cc.shape == "underline") {
        shape = cc.blink ? CursorUnderline : CursorSteadyUnderline;
    } else if (cc.shape == "bar" || cc.shape == "beam") {
        shape = cc.blink ? CursorBar : CursorSteadyBar;
    } else {
        // "block" or anything unrecognized falls back to block
        shape = cc.blink ? CursorBlock : CursorSteadyBlock;
    }
    setDefaultCursorShape(shape);
    setDefaultCursorBlinkEnabled(cc.blink);
}

void TerminalEmulator::resize(int width, int height)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    int oldCols = mWidth;
    mWidth = width;
    mHeight = height;

    if (oldCols != width && !mUsingAltScreen) {
        // Column change on main screen: reflow with cursor tracking
        int oldHistSize = mDocument.historySize();
        Document::CursorTrack ct;
        ct.srcX = mCursorX;
        ct.srcY = oldHistSize + mCursorY;
        ct.dstX = 0;
        ct.dstY = 0;

        mDocument.resize(width, height, &ct);

        int newHistSize = mDocument.historySize();
        mCursorX = std::min(ct.dstX, width - 1);
        mCursorY = std::max(0, std::min(ct.dstY - newHistSize, height - 1));
    } else {
        int oldHistSize = mDocument.historySize();
        Document::CursorTrack ct;
        ct.srcX = mCursorX;
        ct.srcY = oldHistSize + mCursorY;
        ct.dstX = 0;
        ct.dstY = 0;
        mDocument.resize(width, height, &ct);
        if (oldCols == width) {
            // Height-only shrink: top rows pushed to history, adjust cursor to track content.
            // Height-only grow: don't adjust — the shell tracks its own cursor position and
            // the history rows reappearing at the top shouldn't shift the cursor.
            int histDelta = oldHistSize - mDocument.historySize();
            if (histDelta < 0)
                mCursorY += histDelta;
        }
        mCursorX = std::min(mCursorX, width - 1);
        mCursorY = std::max(0, std::min(mCursorY, height - 1));
    }

    mAltGrid.resize(width, height);
    mScrollTop = 0;
    mScrollBottom = height;
    mViewportOffset = std::clamp(mViewportOffset, 0, mDocument.historySize());
    mWrapPending = false;
    mSavedCursorX = std::min(mSavedCursorX, width - 1);
    mSavedCursorY = std::min(mSavedCursorY, height - 1);
    mSavedWrapPending = false;
    clearSelection();
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::scrollUpInRegion(int n)
{
    IGrid& g = grid();
    // Document::scrollUp handles history push internally when top == 0
    if (!mUsingAltScreen && mViewportOffset > 0 && mScrollTop == 0) {
        // Will gain n history rows from the scroll
        mViewportOffset += n;
    }
    g.scrollUp(mScrollTop, mScrollBottom, n);
    if (!mUsingAltScreen && mViewportOffset > 0) {
        mViewportOffset = std::min(mViewportOffset, mDocument.historySize());
    }
}

bool TerminalEmulator::copyViewportRow(int viewRow, std::span<Cell> dst) const
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);

    assert(static_cast<int>(dst.size()) == mWidth);

    const Cell* src = nullptr;
    if (mViewportOffset == 0) {
        src = grid().row(viewRow);
    } else {
        int histSize = mDocument.historySize();
        int logicalRow = histSize - mViewportOffset + viewRow;
        if (logicalRow < histSize) {
            src = mDocument.historyRow(logicalRow);
        } else {
            src = grid().row(logicalRow - histSize);
        }
    }

    if (!src) return false;
    std::memcpy(dst.data(), src, sizeof(Cell) * static_cast<size_t>(mWidth));
    return true;
}

void TerminalEmulator::scrollViewport(int delta)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    int oldOffset = mViewportOffset;
    // delta > 0 means scroll into history, delta < 0 toward live
    mViewportOffset = std::clamp(mViewportOffset + delta, 0, mDocument.historySize());
    if (mViewportOffset != oldOffset) {
        grid().markAllDirty();
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
    }
}

void TerminalEmulator::resetViewport()
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (mViewportOffset != 0) {
        mViewportOffset = 0;
        grid().markAllDirty();
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
    }
}

// Helper: semantic type of the first non-blank cell in a row, or the first cell
// if all blank. Returns Output for rows that have never seen OSC 133 content.
static CellAttrs::SemanticType rowSemanticType(const Cell* row, int cols)
{
    for (int c = 0; c < cols; ++c) {
        if (row[c].wc != 0) return row[c].attrs.semanticType();
    }
    return row[0].attrs.semanticType();
}

void TerminalEmulator::scrollToPrompt(int direction)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Walk cell tags directly — reflow-correct because tags ride with cells.
    // A "prompt-start row" is a row tagged Prompt whose predecessor is not.
    int histSize = mDocument.historySize();
    int totalRows = histSize + mHeight;
    int currentAbsRow = histSize - mViewportOffset;

    auto getRow = [&](int absRow) -> const Cell* {
        return absRow < histSize
            ? mDocument.historyRow(absRow)
            : mDocument.row(absRow - histSize);
    };

    auto isPromptStart = [&](int absRow) {
        const Cell* row = getRow(absRow);
        if (!row || rowSemanticType(row, mWidth) != CellAttrs::Prompt) return false;
        if (absRow == 0) return true;
        const Cell* prev = getRow(absRow - 1);
        return !prev || rowSemanticType(prev, mWidth) != CellAttrs::Prompt;
    };

    auto scrollTo = [&](int targetAbsRow) {
        int newOffset = histSize - targetAbsRow;
        if (newOffset <= 0) {
            resetViewport();
        } else {
            mViewportOffset = std::clamp(newOffset, 0, histSize);
            grid().markAllDirty();
            if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
        }
    };

    if (direction < 0) {
        for (int r = currentAbsRow - 1; r >= 0; --r) {
            if (isPromptStart(r)) { scrollTo(r); return; }
        }
    } else {
        for (int r = currentAbsRow + 1; r < totalRows; ++r) {
            if (isPromptStart(r)) { scrollTo(r); return; }
        }
        resetViewport();
    }
}

void TerminalEmulator::selectCommandOutput()
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Walk cells around the viewport pivot to find a contiguous Output region.
    int histSize = mDocument.historySize();
    int totalRows = histSize + mHeight;
    int pivot = histSize - mViewportOffset + (mHeight / 2);
    if (pivot < 0 || pivot >= totalRows) return;

    auto getRow = [&](int absRow) -> const Cell* {
        return absRow < histSize
            ? mDocument.historyRow(absRow)
            : mDocument.row(absRow - histSize);
    };
    auto rowType = [&](int absRow) -> CellAttrs::SemanticType {
        const Cell* row = getRow(absRow);
        return row ? rowSemanticType(row, mWidth) : CellAttrs::Output;
    };

    // Pivot must be on an Output row to anchor a selection.
    if (rowType(pivot) != CellAttrs::Output) return;

    int outStart = pivot;
    while (outStart > 0 && rowType(outStart - 1) == CellAttrs::Output) --outStart;
    int outEnd = pivot;
    while (outEnd + 1 < totalRows && rowType(outEnd + 1) == CellAttrs::Output) ++outEnd;

    startSelection(0, outStart);
    updateSelection(mWidth - 1, outEnd);
    finalizeSelection();

    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard) {
        mCallbacks.copyToClipboard(text);
    }
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

// ============================================================================
// OSC 133 / shell-integration state machine.
// ============================================================================

int TerminalEmulator::absoluteRowFromScreen(int screenRow) const
{
    if (mUsingAltScreen) return screenRow;      // alt screen has no scrollback
    return mDocument.historySize() + std::max(0, std::min(screenRow, mHeight - 1));
}

const TerminalEmulator::CommandRecord* TerminalEmulator::lastCommand() const
{
    // Walk backwards looking for the last completed record, skipping any
    // in-flight ones at the tail. (JS paneCommands does the same — having
    // consistent semantics across the two APIs avoids subtle caller bugs.)
    for (auto it = mCommandRing.rbegin(); it != mCommandRing.rend(); ++it) {
        if (it->complete) return &*it;
    }
    return nullptr;
}

TerminalEmulator::CommandRecord* TerminalEmulator::inProgressCommandMut()
{
    if (!mCommandInProgress || mCommandRing.empty()) return nullptr;
    CommandRecord& r = mCommandRing.back();
    return r.complete ? nullptr : &r;
}

void TerminalEmulator::startCommand(int absRow, int col)
{
    uint64_t rowId = mDocument.rowIdForAbs(absRow);
    if (CommandRecord* existing = inProgressCommandMut()) {
        if (existing->outputStartCol < 0) {
            // Collapse case: shell re-emits A before output has begun. This is a
            // prompt redraw (multi-line header collapses into one line) — relocate
            // the in-flight record instead of creating a new one.
            existing->promptStartRowId  = rowId;
            existing->promptStartCol    = col;
            existing->commandStartRowId = 0; existing->commandStartCol = -1;
            return;
        }
        // Output already started but no D seen — shell moved on (cancelled or
        // just missing the D). Finalize as complete without exit code, then
        // create a fresh record.
        finishCommand(absRow, col, std::nullopt);
    }
    CommandRecord r;
    r.id                = mNextCommandId++;
    r.promptStartRowId  = rowId;
    r.promptStartCol    = col;
    r.cwd               = mCurrentCwd;
    mCommandRing.push_back(std::move(r));
    while (mCommandRing.size() > COMMAND_RING_MAX) mCommandRing.pop_front();
    mCommandInProgress = true;
}

void TerminalEmulator::markCommandInput(int absRow, int col)
{
    if (CommandRecord* r = inProgressCommandMut()) {
        r->commandStartRowId = mDocument.rowIdForAbs(absRow);
        r->commandStartCol   = col;
    }
}

void TerminalEmulator::markCommandOutput(int absRow, int col)
{
    CommandRecord* r = inProgressCommandMut();
    if (!r) return;
    r->outputStartRowId = mDocument.rowIdForAbs(absRow);
    r->outputStartCol   = col;
    r->startMs          = mono();
}

void TerminalEmulator::finishCommand(int absRow, int col, std::optional<int> exitCode)
{
    CommandRecord* r = inProgressCommandMut();
    if (!r) { mCommandInProgress = false; return; }
    r->outputEndRowId = mDocument.rowIdForAbs(absRow);
    r->outputEndCol   = col;
    r->endMs          = mono();
    r->exitCode       = exitCode;
    r->complete       = true;
    mCommandInProgress = false;
    if (mCallbacks.event) {
        mCallbacks.event(this, static_cast<int>(CommandComplete), r);
    }
}


std::string TerminalEmulator::serializeScrollback() const
{
    std::string result;
    int histSize = mDocument.historySize();

    // Serialize all rows: history first, then screen
    for (int i = 0; i < histSize + mHeight; ++i) {
        const Cell* row;
        if (i < histSize) {
            row = mDocument.historyRow(i);
        } else {
            row = mDocument.row(i - histSize);
        }
        if (!row) continue;

        // Find effective width (trim trailing blank cells)
        int effectiveWidth = mWidth;
        while (effectiveWidth > 0 && row[effectiveWidth - 1].wc == 0) {
            effectiveWidth--;
        }

        // Convert cells to UTF-8
        for (int c = 0; c < effectiveWidth; ++c) {
            if (row[c].attrs.wideSpacer()) continue;
            char32_t cp = row[c].wc;
            if (cp == 0) cp = ' ';
            if (cp < 0x80) {
                result += static_cast<char>(cp);
            } else {
                char buf[4];
                int n = utf8::encode(cp, buf);
                result.append(buf, n);
            }
        }
        result += '\n';
    }
    return result;
}

void TerminalEmulator::advanceCursorToNewLine()
{
    // Mark current row as continued (soft-wrapped) — main screen only
    if (!mUsingAltScreen && mCursorY >= 0 && mCursorY < mHeight) {
        mDocument.setRowContinued(mCursorY, true);
    }
    // Line wrap: move to column 0 of next line, scrolling if needed
    mCursorX = 0;
    mCursorY++;
    if (mCursorY >= mScrollBottom) {
        mCursorY = mScrollBottom - 1;
        scrollUpInRegion(1);
    }
}

void TerminalEmulator::lineFeed()
{
    // LF: move cursor down one line, column unchanged. Scroll if at bottom of scroll region.
    mCursorY++;
    if (mCursorY >= mScrollBottom) {
        mCursorY = mScrollBottom - 1;
        scrollUpInRegion(1);
    }
}

void TerminalEmulator::injectData(const char* buf, size_t len_)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("injectData: \"{}\"", toPrintable(buf, static_cast<int>(len_)));
    obs::notifyParse(len_);
    const int len = static_cast<int>(len_);

    auto resetToNormal = [this]() {
        assert(mState == InEscape || mState == InStringSequence);
        mState = Normal;
        mEscapeIndex = 0;
#ifndef NDEBUG
        memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
#endif
    };

    for (int i=0; i<len; ++i) {
        IGrid& g = grid();
        switch (mState) {
        case Normal:
            switch (buf[i]) {
            case 0x1b: // escape
                mState = InEscape;
                assert(mEscapeIndex == 0);
                break;
            case '\n':
                mWrapPending = false;
                lineFeed();
                break;
            case '\r':
                mCursorX = 0;
                mWrapPending = false;
                break;
            case '\b':
                if (mCursorX > 0)
                    --mCursorX;
                mWrapPending = false;
                break;
            case '\t': {
                // Tab: advance to next 8-column boundary
                // Note: tab preserves lcf/wrapPending per xterm behavior
                int nextTab = (mCursorX / 8 + 1) * 8;
                mCursorX = std::min(nextTab, mWidth - 1);
                break;
            }
            case '\v':
            case '\f':
                // Vertical tab and form feed act as line feeds
                mWrapPending = false;
                lineFeed();
                break;
            case '\a':
                break;
            default:
                if (static_cast<unsigned char>(buf[i]) >= 0x80) {
                    assert(mUtf8Index == 0);
                    mUtf8Buffer[mUtf8Index++] = buf[i];
                    mState = InUtf8;
                } else {
                    // ASCII character — write to cell grid
                    mLastPrintedChar = static_cast<char32_t>(buf[i]);
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        if (mInsertMode) {
                            g.insertChars(mCursorY, mCursorX, 1);
                        }
                        g.cell(mCursorX, mCursorY) = Cell{static_cast<char32_t>(buf[i]), mCurrentAttrs};
                        g.clearExtra(mCursorX, mCursorY);
                        if (mActiveHyperlinkId || mCurrentUnderlineColor) {
                            CellExtra& ex = g.ensureExtra(mCursorX, mCursorY);
                            ex.hyperlinkId = mActiveHyperlinkId;
                            ex.underlineColor = mCurrentUnderlineColor;
                        }
                        g.markRowDirty(mCursorY);
                    }
                    mCursorX++;
                    if (mCursorX >= mWidth) {
                        mCursorX = mWidth - 1;
                        if (mAutoWrap) mWrapPending = true;
                    }
                }
                break;
            }
            break;
        case InUtf8: {
            assert(mUtf8Index > 0 && mUtf8Index < 6);
            if ((buf[i] & 0xC0) != 0x80) {
                // Not a continuation byte — bad UTF-8
                sLog().error("Bad utf8 sequence (non-continuation byte)");
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mState = Normal;
                --i; // reprocess
                break;
            }
            mUtf8Buffer[mUtf8Index++] = buf[i];

            int expected = utf8::seqLen(static_cast<uint8_t>(mUtf8Buffer[0]));

            if (mUtf8Index == expected) {
                int consumed = 0;
                char32_t cp = utf8::decode(mUtf8Buffer, expected, consumed);

                int w = wcwidth(cp);
                if (w < 0) w = 0; // non-printable control char — skip
                if (w == 0) {
                    // Zero-width / combining character — attach to previous cell
                    if (cp == 0xFE0F && mLastPrintedX >= 0 && mLastPrintedY >= 0 &&
                        mLastPrintedY < mHeight && mLastPrintedX < mWidth) {
                        CellExtra& ex = g.ensureExtra(mLastPrintedX, mLastPrintedY);
                        ex.combiningCp = cp;

                        // Widen the previous cell if it was single-width and there's room
                        Cell& prevCell = g.cell(mLastPrintedX, mLastPrintedY);
                        if (!prevCell.attrs.wide() && mLastPrintedY == mCursorY && !mWrapPending) {
                            prevCell.attrs.setWide(true);
                            CellAttrs spacerAttrs = mCurrentAttrs;
                            spacerAttrs.setWideSpacer(true);
                            g.cell(mCursorX, mCursorY) = Cell{0, spacerAttrs};
                            g.clearExtra(mCursorX, mCursorY);
                            mCursorX++;
                            if (mCursorX >= mWidth) {
                                mCursorX = mWidth - 1;
                                if (mAutoWrap) mWrapPending = true;
                            }
                        }

                        g.markRowDirty(mLastPrintedY);
                    }
                } else if (w == 2) {
                    // Wide character: needs two cells
                    mLastPrintedChar = cp;
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX + 1 >= mWidth) {
                        // Not enough room — fill current cell with space and wrap
                        if (mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                            g.cell(mCursorX, mCursorY) = Cell{' ', mCurrentAttrs};
                            g.markRowDirty(mCursorY);
                        }
                        advanceCursorToNewLine();
                    }
                    if (mCursorX >= 0 && mCursorX + 1 < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        if (mInsertMode) g.insertChars(mCursorY, mCursorX, 2);
                        mLastPrintedX = mCursorX;
                        mLastPrintedY = mCursorY;
                        CellAttrs wideAttrs = mCurrentAttrs;
                        wideAttrs.setWide(true);
                        g.cell(mCursorX, mCursorY) = Cell{cp, wideAttrs};
                        g.clearExtra(mCursorX, mCursorY);
                        if (mActiveHyperlinkId || mCurrentUnderlineColor) {
                            CellExtra& ex = g.ensureExtra(mCursorX, mCursorY);
                            ex.hyperlinkId = mActiveHyperlinkId;
                            ex.underlineColor = mCurrentUnderlineColor;
                        }
                        CellAttrs spacerAttrs = mCurrentAttrs;
                        spacerAttrs.setWideSpacer(true);
                        g.cell(mCursorX + 1, mCursorY) = Cell{0, spacerAttrs};
                        g.clearExtra(mCursorX + 1, mCursorY);
                        g.markRowDirty(mCursorY);
                    }
                    mCursorX += 2;
                    if (mCursorX >= mWidth) {
                        mCursorX = mWidth - 1;
                        if (mAutoWrap) mWrapPending = true;
                    }
                } else {
                    // Normal single-width character
                    mLastPrintedChar = cp;
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        if (mInsertMode) g.insertChars(mCursorY, mCursorX, 1);
                        mLastPrintedX = mCursorX;
                        mLastPrintedY = mCursorY;
                        g.cell(mCursorX, mCursorY) = Cell{cp, mCurrentAttrs};
                        g.clearExtra(mCursorX, mCursorY);
                        if (mActiveHyperlinkId || mCurrentUnderlineColor) {
                            CellExtra& ex = g.ensureExtra(mCursorX, mCursorY);
                            ex.hyperlinkId = mActiveHyperlinkId;
                            ex.underlineColor = mCurrentUnderlineColor;
                        }
                        g.markRowDirty(mCursorY);
                    }
                    mCursorX++;
                    if (mCursorX >= mWidth) {
                        mCursorX = mWidth - 1;
                        if (mAutoWrap) mWrapPending = true;
                    }
                }

                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mState = Normal;
            } else if (mUtf8Index >= 6 || mUtf8Index > expected) {
                sLog().error("Bad utf8 (overlong sequence)");
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mState = Normal;
            }
            break;
        }
        case InEscape:
            if (mEscapeIndex >= static_cast<int>(sizeof(mEscapeBuffer))) {
                sLog().error("Escape buffer overflow");
                resetToNormal();
                break;
            }
            mEscapeBuffer[mEscapeIndex++] = buf[i];
            if (sLog().should_log(spdlog::level::debug))
                sLog().debug("Adding escape byte [{}] {} {} -> {}", mEscapeIndex - 1,
                      toPrintable(buf + i, 1),
                      i, toPrintable(mEscapeBuffer, mEscapeIndex));
            switch (mEscapeBuffer[0]) {
            case SS2:
            case SS3:
            case ST:
                if (mWasInStringSequence) {
                    processStringSequence();
                    mStringSequence.clear();
                    mWasInStringSequence = false;
                }
                resetToNormal();
                break;
            case DCS:
            case OSX:
            case SOS:
            case PM:
            case APC:
                mStringSequenceType = mEscapeBuffer[0];
                mStringSequence.clear();
                mWasInStringSequence = false;
                mState = InStringSequence;
                break;
            case CSI:
                if (mEscapeIndex > 1) {
                    if (buf[i] >= 0x40 && buf[i] <= 0x7e) {
                        processCSI();
                        assert(mState == Normal);
                    } else if (mEscapeIndex >= static_cast<int>(sizeof(mEscapeBuffer))) {
                        sLog().error("CSI sequence is too long {}", sizeof(mEscapeBuffer));
                        resetToNormal();
                    } else if (buf[i] < 0x20 || buf[i] > 0x3f) {
                        sLog().error("Invalid CSI sequence {:#04x} character", static_cast<unsigned char>(buf[i]));
                        resetToNormal();
                    }
                }
                break;
            case RIS:
                // Full reset to initial state
                mWrapPending = false;
                mCurrentAttrs.reset();
                mCurrentUnderlineColor = 0;
                mCursorX = 0;
                mCursorY = 0;
                mCursorVisible = true;
                mCursorShape = mDefaultCursorShape;
                mCursorBlinkEnabled = mDefaultCursorBlinkEnabled;
                mScrollTop = 0;
                mScrollBottom = mHeight;
                mCursorKeyMode = false;
                mKeypadMode = false;
                mMouseMode1000 = false;
                mMouseMode1002 = false;
                mMouseMode1003 = false;
                mMouseMode1006 = false;
                mMouseMode1016 = false;
                mAutoWrap = true;
                mInsertMode = false;
                mBracketedPaste = false;
                mFocusReporting = false;
                mSyncOutput = false;
                mColorPreferenceReporting = false;
                if (mUsingAltScreen) {
                    mUsingAltScreen = false;
                    mDocument.markAllDirty();
                }
                mPointerShapeStackMain.clear();
                mPointerShapeStackAlt.clear();
                notifyPointerShapeChanged();
                g.markAllDirty();
                for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
                clearSelection();
                // Reset kitty keyboard protocol state
                mKittyFlags = 0;
                mKittyStackDepthMain = 0;
                mKittyStackDepthAlt = 0;
                memset(mKittyStackMain, 0, sizeof(mKittyStackMain));
                memset(mKittyStackAlt, 0, sizeof(mKittyStackAlt));
                resetToNormal();
                break;
            case VB:
                if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(VisibleBell), nullptr);
                resetToNormal();
                break;
            case '(':  // G0 charset designation — ESC ( X
            case ')':  // G1 charset designation — ESC ) X
                // Need one more byte (the charset designator). If we have it, consume and ignore.
                if (mEscapeIndex >= 2) {
                    resetToNormal();
                }
                // Otherwise wait for the next byte.
                break;
            case DECKPAM:
                mKeypadMode = true;
                resetToNormal();
                break;
            case DECKPNM:
                mKeypadMode = false;
                resetToNormal();
                break;
            case DECSC:
                mSavedCursorX = mCursorX;
                mSavedCursorY = mCursorY;
                mSavedAttrs = mCurrentAttrs;
                mSavedWrapPending = mWrapPending;
                resetToNormal();
                break;
            case DECRC:
                mCursorX = mSavedCursorX;
                mCursorY = mSavedCursorY;
                mCurrentAttrs = mSavedAttrs;
                mWrapPending = mSavedWrapPending;
                resetToNormal();
                break;
            case IND:
                // Index: same as LF
                mWrapPending = false;
                lineFeed();
                resetToNormal();
                break;
            case NEL:
                // Next Line: move to beginning of next line, scroll if needed
                mWrapPending = false;
                mCursorX = 0;
                if (mCursorY == mScrollBottom - 1) {
                    scrollUpInRegion(1);
                } else if (mCursorY < mHeight - 1) {
                    mCursorY++;
                }
                resetToNormal();
                break;
            case RI:
                // Reverse Index: move cursor up one line, scroll down if at top of scroll region
                mWrapPending = false;
                if (mCursorY == mScrollTop) {
                    g.scrollDown(mScrollTop, mScrollBottom, 1);
                } else if (mCursorY > 0) {
                    mCursorY--;
                }
                resetToNormal();
                break;
            default:
                sLog().error("Unknown escape sequence {}", toPrintable(mEscapeBuffer, 1));
                resetToNormal();
                break;
            }
            break;
        case InStringSequence:
            if (buf[i] == '\x07') {
                processStringSequence();
                mStringSequence.clear();
                resetToNormal();
            } else if (buf[i] == 0x1b) {
                // Possible ST terminator (\x1b\\) — transition to InEscape
                mWasInStringSequence = true;
                mState = InEscape;
                mEscapeIndex = 0;
            } else if (mStringSequence.size() < MAX_STRING_SEQUENCE) {
                // Scan ahead for a contiguous run of payload bytes to avoid
                // per-byte std::string append (major perf win for large payloads
                // like kitty graphics base64 data).
                int runStart = i;
                size_t remaining = MAX_STRING_SEQUENCE - mStringSequence.size();
                int limit = std::min(len, static_cast<int>(runStart + remaining));
                while (i + 1 < limit &&
                       buf[i + 1] != '\x07' && buf[i + 1] != 0x1b) {
                    ++i;
                }
                mStringSequence.append(buf + runStart, i - runStart + 1);
            }
            break;
        }
    }

    // Suppress render updates during chunked image transfer to avoid
    // vsync-blocking the event loop while the PTY still has data to deliver.
    if (!mKittyLoading.active) {
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
    }
}

void TerminalEmulator::processCSI()
{
    assert(mState == InEscape);
    assert(mEscapeIndex >= 1);

    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("Processing CSI \"{}\"", toPrintable(mEscapeBuffer, mEscapeIndex));

    auto readCount = [this](int def) {
        if (mEscapeIndex == 2) // no digits
            return def;
        char *end;
        unsigned long l = strtoul(mEscapeBuffer + 1, &end, 10);
        if (end != mEscapeBuffer + mEscapeIndex - 1 || l > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            return -1;
        }
        return static_cast<int>(l);
    };

    // Check for prefix characters '?', '>', '=', '<'
    bool isPrivate = (mEscapeIndex > 1 && mEscapeBuffer[1] == '?');
    bool isSecondary = (mEscapeIndex > 1 && mEscapeBuffer[1] == '>');
    bool isEquals = (mEscapeIndex > 1 && mEscapeBuffer[1] == '=');
    bool isLess = (mEscapeIndex > 1 && mEscapeBuffer[1] == '<');

    Action action;
    char finalByte = mEscapeBuffer[mEscapeIndex - 1];

    switch (finalByte) {
    case CUU:
    case CUD:
    case CUF:
    case CUB:
    case CNL:
    case CPL:
    case CHA:
    case DCH:
    case ICH:
    case IL:
    case DL:
    case ECH:
    case VPA: {
        const int count = readCount(1);
        if (count == -1) {
            sLog().error("Invalid parameters for CSI {}", csiSequenceName(static_cast<CSISequence>(finalByte)));
            break;
        }
        action.count = count;
        switch (finalByte) {
        case CUU: action.type = Action::CursorUp; break;
        case CUD: action.type = Action::CursorDown; break;
        case CUF: action.type = Action::CursorForward; break;
        case CUB: action.type = Action::CursorBack; break;
        case CNL: action.type = Action::CursorNextLine; break;
        case CPL: action.type = Action::CursorPreviousLine; break;
        case CHA: action.type = Action::CursorHorizontalAbsolute; break;
        case DCH: action.type = Action::DeleteChars; break;
        case ICH: action.type = Action::InsertChars; break;
        case IL: action.type = Action::InsertLines; break;
        case DL: action.type = Action::DeleteLines; break;
        case ECH: action.type = Action::EraseChars; break;
        case VPA: action.type = Action::VerticalPositionAbsolute; break;
        }
        break; }
    case HVP:
    case CUP: {
        char *end;
        action.x = action.y = 1;
        action.type = Action::CursorPosition;
        const unsigned long x = strtoul(mEscapeBuffer + 1, &end, 10);
        if (end == mEscapeBuffer + mEscapeIndex - 1) {
            if (mEscapeIndex != 2) {
                if (!x) {
                    sLog().error("Invalid CSI CUP error 1");
                    action.type = Action::Invalid;
                } else {
                    action.x = x;
                }
            }
        } else if (*end == ';') {
            if (mEscapeBuffer[1] != ';') {
                if (x == 0) {
                    sLog().error("Invalid CSI CUP error 2");
                    action.type = Action::Invalid;
                    break;
                }
                action.x = x;
            }
            if (end + 1 < mEscapeBuffer + mEscapeIndex - 1) {
                const unsigned long y = strtoul(end + 1, &end, 10);
                if (end != mEscapeBuffer + mEscapeIndex - 1 || !y) {
                    sLog().error("Invalid CSI CUP error 3");
                    action.type = Action::Invalid;
                    break;
                } else {
                    action.y = y;
                }
            }
        }
        break; }
    case ED:
        switch (readCount(0)) {
        case 0: action.type = Action::ClearToEndOfScreen; break;
        case 1: action.type = Action::ClearToBeginningOfScreen; break;
        case 2: action.type = Action::ClearScreen; break;
        case 3: // Clear screen + scrollback
            action.type = Action::ClearScreen;
            if (!mUsingAltScreen) mDocument.clearHistory();
            break;
        default: sLog().error("Invalid CSI ED {}", readCount(0)); break;
        }
        break;
    case EL:
        switch (readCount(0)) {
        case 0: action.type = Action::ClearToEndOfLine; break;
        case 1: action.type = Action::ClearToBeginningOfLine; break;
        case 2: action.type = Action::ClearLine; break;
        default: sLog().error("Invalid CSI EL {}", readCount(0)); break;
        }
        break;
    case SU: {
        const int n = readCount(1);
        if (n <= 0) {
            sLog().error("Invalid CSI SU {}", n);
        } else {
            action.type = Action::ScrollUp;
            action.count = n;
        }
        break; }
    case SD: {
        const int n = readCount(1);
        if (n <= 0) {
            sLog().error("Invalid CSI SD {}", n);
        } else {
            action.type = Action::ScrollDown;
            action.count = n;
        }
        break; }
    case SGR:
        processSGR();
        break;
    case REP: {
        // Repeat preceding graphic character N times
        const int n = readCount(1);
        if (n > 0 && mLastPrintedChar != 0) {
            IGrid& g = grid();
            int w = wcwidth(mLastPrintedChar);
            if (w < 1) w = 1;
            for (int rep = 0; rep < n; ++rep) {
                if (mWrapPending) {
                    advanceCursorToNewLine();
                    mWrapPending = false;
                }
                if (w == 2) {
                    if (mCursorX + 1 >= mWidth) {
                        if (mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                            g.cell(mCursorX, mCursorY) = Cell{' ', mCurrentAttrs};
                            g.markRowDirty(mCursorY);
                        }
                        advanceCursorToNewLine();
                    }
                    if (mCursorX >= 0 && mCursorX + 1 < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        CellAttrs wideAttrs = mCurrentAttrs;
                        wideAttrs.setWide(true);
                        g.cell(mCursorX, mCursorY) = Cell{mLastPrintedChar, wideAttrs};
                        g.clearExtra(mCursorX, mCursorY);
                        CellAttrs spacerAttrs = mCurrentAttrs;
                        spacerAttrs.setWideSpacer(true);
                        g.cell(mCursorX + 1, mCursorY) = Cell{0, spacerAttrs};
                        g.clearExtra(mCursorX + 1, mCursorY);
                        g.markRowDirty(mCursorY);
                    }
                    mCursorX += 2;
                } else {
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        g.cell(mCursorX, mCursorY) = Cell{mLastPrintedChar, mCurrentAttrs};
                        g.clearExtra(mCursorX, mCursorY);
                        g.markRowDirty(mCursorY);
                    }
                    mCursorX++;
                }
                if (mCursorX >= mWidth) {
                    mCursorX = mWidth - 1;
                    if (mAutoWrap) mWrapPending = true;
                }
            }
        }
        break; }
    case AUX:
        if (mEscapeIndex != 3) {
            sLog().error("Invalid AUX CSI command ({})", mEscapeIndex);
        } else if (mEscapeBuffer[1] == '4') {
            action.type = Action::AUXPortOff;
        } else if (mEscapeBuffer[1] == '5') {
            action.type = Action::AUXPortOn;
        } else {
            sLog().error("Invalid AUX CSI command ({:#04x})", static_cast<unsigned char>(mEscapeBuffer[1]));
        }
        break;
    case DSR:
        if (isPrivate && mEscapeIndex >= 4) {
            // CSI ? Ps n — private DSR queries
            char* end;
            int ps = static_cast<int>(strtoul(mEscapeBuffer + 2, &end, 10));
            if (ps == 996) {
                // Color preference query: 1=dark, 2=light
                bool isDark = true;
                if (mCallbacks.isDarkMode) isDark = mCallbacks.isDarkMode();
                char response[16];
                int rlen = snprintf(response, sizeof(response), "\x1b[?997;%dn", isDark ? 1 : 2);
                writeToOutput(response, rlen);
            } else {
                sLog().warn("Unhandled private DSR {}", ps);
            }
        } else if (mEscapeIndex == 3 && mEscapeBuffer[1] == '5') {
            // Device status report: respond "OK"
            writeToOutput("\x1b[0n", 4);
        } else if (mEscapeIndex == 3 && mEscapeBuffer[1] == '6') {
            // Report cursor position: ESC [ row ; col R
            char response[32];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;%dR",
                                mCursorY + 1, mCursorX + 1);
            writeToOutput(response, rlen);
        } else {
            sLog().warn("Unhandled DSR: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case SCP:
        if (isPrivate) {
            // CSI ? Pm s — XTSAVE: save DEC private mode values
            std::vector<int> modes;
            const char* p = mEscapeBuffer + 2;
            const char* end = mEscapeBuffer + mEscapeIndex - 1;
            while (p < end) {
                char* next;
                unsigned long v = strtoul(p, &next, 10);
                if (next == p) break;
                modes.push_back(static_cast<int>(v));
                p = next;
                if (p < end && *p == ';') ++p;
            }
            savePrivateModes(modes);
        } else if (mEscapeIndex == 2) {
            action.type = Action::SaveCursorPosition;
        } else {
            sLog().warn("Ignoring CSI s variant: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case RCP:
        if (isPrivate) {
            // CSI ? u — query current keyboard flags
            kittyQueryFlags();
        } else if (isSecondary) {
            // CSI > flags u — push flags onto stack
            char *end;
            uint8_t flags = static_cast<uint8_t>(strtoul(mEscapeBuffer + 2, &end, 10));
            kittyPushFlags(flags & 0x1F);
        } else if (isEquals) {
            // CSI = flags u — set flags
            // CSI = flags ; mode u — set flags with mode (1=replace, 2=OR, 3=AND NOT)
            char *end;
            uint8_t flags = static_cast<uint8_t>(strtoul(mEscapeBuffer + 2, &end, 10));
            int mode = 1;
            if (*end == ';') {
                mode = static_cast<int>(strtoul(end + 1, &end, 10));
            }
            kittySetFlags(flags & 0x1F, mode);
        } else if (isLess) {
            // CSI < number u — pop N entries from stack
            char *end;
            int count = 1;
            if (mEscapeIndex > 3) {
                count = static_cast<int>(strtoul(mEscapeBuffer + 2, &end, 10));
                if (count < 1) count = 1;
            }
            kittyPopFlags(count);
        } else if (mEscapeIndex == 2) {
            action.type = Action::RestoreCursorPosition;
        } else {
            sLog().warn("Ignoring CSI u variant: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case SM: // Set Mode
        if (isPrivate) {
            action.type = Action::SetMode;
            // Parse the mode number
            char *end;
            action.count = strtoul(mEscapeBuffer + 2, &end, 10); // skip "[?"
        } else {
            char *end;
            unsigned long mode = strtoul(mEscapeBuffer + 1, &end, 10); // skip "["
            if (mode == 4) {
                mInsertMode = true;
            } else {
                sLog().warn("Ignoring non-private SM: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
            }
        }
        break;
    case RM: // Reset Mode
        if (isPrivate) {
            action.type = Action::ResetMode;
            char *end;
            action.count = strtoul(mEscapeBuffer + 2, &end, 10);
        } else {
            char *end;
            unsigned long mode = strtoul(mEscapeBuffer + 1, &end, 10); // skip "["
            if (mode == 4) {
                mInsertMode = false;
            } else {
                sLog().warn("Ignoring non-private RM: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
            }
        }
        break;
    case DECSTBM: { // Set scrolling region (or XTRESTORE if private prefix)
        if (isPrivate) {
            // CSI ? Pm r — XTRESTORE: restore DEC private mode values
            std::vector<int> modes;
            const char* p = mEscapeBuffer + 2;
            const char* end = mEscapeBuffer + mEscapeIndex - 1;
            while (p < end) {
                char* next;
                unsigned long v = strtoul(p, &next, 10);
                if (next == p) break;
                modes.push_back(static_cast<int>(v));
                p = next;
                if (p < end && *p == ';') ++p;
            }
            restorePrivateModes(modes);
            break;
        }
        // CSI Pt ; Pb r
        int top = 1, bottom = mHeight;
        char *end;
        unsigned long t = strtoul(mEscapeBuffer + 1, &end, 10);
        if (end != mEscapeBuffer + 1 && t > 0) top = static_cast<int>(t);
        if (*end == ';') {
            unsigned long b = strtoul(end + 1, &end, 10);
            if (b > 0) bottom = static_cast<int>(b);
        }
        mScrollTop = std::max(0, top - 1);
        mScrollBottom = std::min(mHeight, bottom);
        if (mScrollTop >= mScrollBottom) {
            mScrollTop = 0;
            mScrollBottom = mHeight;
        }
        mCursorX = 0;
        mCursorY = mScrollTop;
        break; }
    case 'c': // Device Attributes
        if (isSecondary) {
            // CSI > c — Secondary DA: VT500-class, xterm version 2500
            writeToOutput("\x1b[>64;2500;0c", 13);
        } else if (mEscapeIndex == 2 || (mEscapeIndex == 3 && mEscapeBuffer[1] == '0')) {
            // CSI c or CSI 0 c — Primary DA: VT420 with common features
            writeToOutput("\x1b[?64;1;2;6;22c", 16);
        } else {
            sLog().warn("Ignoring DA variant: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case 'q':
        if (isSecondary) {
            // CSI > q — XTVERSION: report terminal name/version
            static const char xtver[] = "\x1bP>|MasterBandit(0.1)\x1b\\";
            writeToOutput(xtver, sizeof(xtver) - 1);
        } else {
            // CSI Ps SP q — DECSCUSR (Set Cursor Style)
            {
                int ps = 0;
                if (mEscapeIndex > 3) {
                    // Parse parameter between '[' and ' q'
                    char* end;
                    ps = static_cast<int>(strtoul(mEscapeBuffer + 1, &end, 10));
                }
                switch (ps) {
                case 0: case 1: mCursorShape = CursorBlock; break;
                case 2: mCursorShape = CursorSteadyBlock; break;
                case 3: mCursorShape = CursorUnderline; break;
                case 4: mCursorShape = CursorSteadyUnderline; break;
                case 5: mCursorShape = CursorBar; break;
                case 6: mCursorShape = CursorSteadyBar; break;
                default: break;
                }
            }
        }
        break;
    case 'x': {
        // CSI Ps x with no intermediate is DECREQTPARM (Request Terminal Parameters).
        // With an intermediate ($, *, ' ...) it is a different command (DECFRA/DECSACE/etc).
        char prev = mEscapeIndex >= 2 ? mEscapeBuffer[mEscapeIndex - 2] : 0;
        if (prev >= 0x20 && prev <= 0x2f) {
            sLog().warn("Ignoring CSI x with intermediate: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
            break;
        }
        int ps = 0;
        if (mEscapeIndex > 2) {
            char* end;
            ps = static_cast<int>(strtoul(mEscapeBuffer + 1, &end, 10));
        }
        if (ps == 0 || ps == 1) {
            // Reply: CSI Psol;1;1;128;128;1;0 x
            //   Psol = 2 (unsolicited reply to req=0) or 3 (solicited reply to req=1)
            //   par=1 (no parity), nbits=1 (8 bits), xspeed=rspeed=128 (19200 baud),
            //   clkmul=1, flags=0
            int psol = (ps == 0) ? 2 : 3;
            char response[48];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;1;1;128;128;1;0x", psol);
            writeToOutput(response, rlen);
        }
        break; }
    case 't': {
        // XTWINOPS — only handle push/pop title (22/23)
        char* end;
        int op = static_cast<int>(strtoul(mEscapeBuffer + 1, &end, 10));
        if (op == 22) {
            // Push title: duplicate top of stack
            if (!mTitleStack.empty() && mTitleStack.size() < TITLE_STACK_MAX)
                mTitleStack.push_back(mTitleStack.back());
        } else if (op == 23) {
            // Pop title: restore previous, or clear if last entry
            if (mTitleStack.size() > 1) {
                mTitleStack.pop_back();
                if (mCallbacks.onTitleChanged)
                    mCallbacks.onTitleChanged(mTitleStack.back());
            } else if (!mTitleStack.empty()) {
                mTitleStack.clear();
                if (mCallbacks.onTitleChanged)
                    mCallbacks.onTitleChanged(std::string{});
            }
        }
        break;
    }
    case 'p': {
        // CSI ? Ps $ p — DECRQM (Request Mode, private)
        // Response: CSI ? Ps ; Pm $ y
        //   Pm: 0=not recognized, 1=set, 2=reset
        if (isPrivate && mEscapeIndex >= 5 &&
            mEscapeBuffer[mEscapeIndex - 2] == '$') {
            char* end;
            int ps = static_cast<int>(strtoul(mEscapeBuffer + 2, &end, 10));
            int pm = 0; // not recognized
            switch (ps) {
            case 1:    pm = mCursorKeyMode ? 1 : 2; break;
            case 7:    pm = mAutoWrap ? 1 : 2; break;
            case 12:   pm = mCursorBlinkEnabled ? 1 : 2; break;
            case 25:   pm = mCursorVisible ? 1 : 2; break;
            case 1000: pm = mMouseMode1000 ? 1 : 2; break;
            case 1002: pm = mMouseMode1002 ? 1 : 2; break;
            case 1003: pm = mMouseMode1003 ? 1 : 2; break;
            case 1006: pm = mMouseMode1006 ? 1 : 2; break;
            case 1016: pm = mMouseMode1016 ? 1 : 2; break;
            case 1004: pm = mFocusReporting ? 1 : 2; break;
            case 1049: pm = mUsingAltScreen ? 1 : 2; break;
            case 2004: pm = mBracketedPaste ? 1 : 2; break;
            case 2026: pm = mSyncOutput ? 1 : 2; break;
            case 2031: pm = mColorPreferenceReporting ? 1 : 2; break;
            default:
                pm = 0;
                sLog().warn("DECRQM: unrecognized private mode {}", ps);
                break;
            }
            char response[32];
            int rlen = snprintf(response, sizeof(response),
                "\x1b[?%d;%d$y", ps, pm);
            writeToOutput(response, rlen);
        } else {
            sLog().warn("Ignoring CSI p variant: \"{}\"", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    }
    default:
        sLog().error("Unknown CSI final byte {:#x}: \"{}\"", static_cast<unsigned char>(finalByte), toPrintable(mEscapeBuffer, mEscapeIndex));
        break;
    }

    if (action.type != Action::Invalid)
        onAction(&action);

    mEscapeIndex = 0;
#ifndef NDEBUG
    memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
#endif
    mState = Normal;
}

void TerminalEmulator::savePrivateModes(const std::vector<int>& modes)
{
    static constexpr int kKnownModes[] = {
        1, 7, 12, 25, 1000, 1002, 1003, 1004, 1006, 1016, 2004, 2026, 2031
    };
    auto saveOne = [this](int m) {
        switch (m) {
        case 1:    mSavedPrivateModes[1]    = mCursorKeyMode; break;
        case 7:    mSavedPrivateModes[7]    = mAutoWrap; break;
        case 12:   mSavedPrivateModes[12]   = mCursorBlinkEnabled; break;
        case 25:   mSavedPrivateModes[25]   = mCursorVisible; break;
        case 1000: mSavedPrivateModes[1000] = mMouseMode1000; break;
        case 1002: mSavedPrivateModes[1002] = mMouseMode1002; break;
        case 1003: mSavedPrivateModes[1003] = mMouseMode1003; break;
        case 1004: mSavedPrivateModes[1004] = mFocusReporting; break;
        case 1006: mSavedPrivateModes[1006] = mMouseMode1006; break;
        case 1016: mSavedPrivateModes[1016] = mMouseMode1016; break;
        case 2004: mSavedPrivateModes[2004] = mBracketedPaste; break;
        case 2026: mSavedPrivateModes[2026] = mSyncOutput; break;
        case 2031: mSavedPrivateModes[2031] = mColorPreferenceReporting; break;
        default: break;
        }
    };
    if (modes.empty()) {
        for (int m : kKnownModes) saveOne(m);
    } else {
        for (int m : modes) saveOne(m);
    }
}

void TerminalEmulator::restorePrivateModes(const std::vector<int>& modes)
{
    auto restoreOne = [this](int m) {
        auto it = mSavedPrivateModes.find(m);
        if (it == mSavedPrivateModes.end()) return;
        bool v = it->second;
        switch (m) {
        case 1:    mCursorKeyMode             = v; break;
        case 7:    mAutoWrap                  = v; break;
        case 12:   mCursorBlinkEnabled        = v; break;
        case 25:   mCursorVisible             = v; break;
        case 1000: mMouseMode1000             = v; break;
        case 1002: mMouseMode1002             = v; break;
        case 1003: mMouseMode1003             = v; break;
        case 1004: mFocusReporting            = v; break;
        case 1006: mMouseMode1006             = v; break;
        case 1016: mMouseMode1016             = v; break;
        case 2004: mBracketedPaste            = v; break;
        case 2026: mSyncOutput                = v; break;
        case 2031: mColorPreferenceReporting  = v; break;
        default: break;
        }
    };
    if (modes.empty()) {
        for (auto& kv : mSavedPrivateModes) restoreOne(kv.first);
    } else {
        for (int m : modes) restoreOne(m);
    }
}

void TerminalEmulator::onAction(const Action *action)
{
    assert(action);
    assert(action->type != Action::Invalid);
    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("Got action {} {} {} {}", Action::typeName(action->type), action->count, action->x, action->y);

    // Any CSI action clears pending autowrap (save old state for SCP)
    bool savedWrapPending = mWrapPending;
    mWrapPending = false;

    IGrid& g = grid();

    switch (action->type) {
    case Action::CursorUp:
        mCursorY = std::max(0, mCursorY - action->count);
        break;
    case Action::CursorDown:
        mCursorY = std::min(mHeight - 1, mCursorY + action->count);
        break;
    case Action::CursorForward:
        mCursorX = std::min(mWidth - 1, mCursorX + action->count);
        break;
    case Action::CursorBack:
        mCursorX = std::max(0, mCursorX - action->count);
        break;
    case Action::CursorNextLine:
        mCursorY = std::min(mHeight - 1, mCursorY + action->count);
        mCursorX = 0;
        break;
    case Action::CursorPreviousLine:
        mCursorY = std::max(0, mCursorY - action->count);
        mCursorX = 0;
        break;
    case Action::CursorHorizontalAbsolute:
        mCursorX = std::clamp(action->count - 1, 0, mWidth - 1);
        break;
    case Action::CursorPosition:
        // CUP: action.x = row (1-based), action.y = col (1-based) — note x/y are swapped vs screen coords
        mCursorY = std::clamp(action->x - 1, 0, mHeight - 1);
        mCursorX = std::clamp(action->y - 1, 0, mWidth - 1);
        break;
    case Action::ClearScreen:
        for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
        mCursorX = 0;
        mCursorY = 0;
        break;
    case Action::ClearToEndOfScreen:
        // Clear from cursor to end of line, then all lines below
        g.clearRow(mCursorY, mCursorX, mWidth);
        for (int r = mCursorY + 1; r < g.rows(); ++r) g.clearRow(r);
        break;
    case Action::ClearToBeginningOfScreen:
        // Clear from start to cursor, plus all lines above
        for (int r = 0; r < mCursorY; ++r) g.clearRow(r);
        g.clearRow(mCursorY, 0, mCursorX + 1);
        break;
    case Action::ClearLine:
        g.clearRow(mCursorY);
        break;
    case Action::ClearToEndOfLine:
        g.clearRow(mCursorY, mCursorX, mWidth);
        break;
    case Action::ClearToBeginningOfLine:
        g.clearRow(mCursorY, 0, mCursorX + 1);
        break;
    case Action::DeleteChars:
        g.deleteChars(mCursorY, mCursorX, action->count);
        break;
    case Action::InsertChars:
        g.insertChars(mCursorY, mCursorX, action->count);
        break;
    case Action::InsertLines:
        // IL: insert blank lines at cursor, pushing existing lines down within scroll region
        if (mCursorY >= mScrollTop && mCursorY < mScrollBottom) {
            g.scrollDown(mCursorY, mScrollBottom, action->count);
        }
        break;
    case Action::DeleteLines:
        // DL: delete lines at cursor, pulling lines up within scroll region
        if (mCursorY >= mScrollTop && mCursorY < mScrollBottom) {
            g.scrollUp(mCursorY, mScrollBottom, action->count);
        }
        break;
    case Action::EraseChars:
        // ECH: erase N chars at cursor without moving it
        g.clearRow(mCursorY, mCursorX, std::min(mCursorX + action->count, mWidth));
        break;
    case Action::VerticalPositionAbsolute:
        mCursorY = std::clamp(action->count - 1, 0, mHeight - 1);
        break;
    case Action::ScrollUp:
        g.scrollUp(mScrollTop, mScrollBottom, action->count);
        break;
    case Action::ScrollDown:
        g.scrollDown(mScrollTop, mScrollBottom, action->count);
        break;
    case Action::SaveCursorPosition:
        mSavedCursorX = mCursorX;
        mSavedCursorY = mCursorY;
        mSavedAttrs = mCurrentAttrs;
        mSavedWrapPending = savedWrapPending;
        break;
    case Action::RestoreCursorPosition:
        mCursorX = mSavedCursorX;
        mCursorY = mSavedCursorY;
        mCurrentAttrs = mSavedAttrs;
        mWrapPending = mSavedWrapPending;
        break;
    case Action::SetMode:
        switch (action->count) {
        case 1: // DECCKM: application cursor keys
            mCursorKeyMode = true;
            break;
        case 7: // DECAWM: autowrap
            mAutoWrap = true;
            break;
        case 12: // ATT610: cursor blink on
            mCursorBlinkEnabled = true;
            break;
        case 25: // DECTCEM: show cursor
            mCursorVisible = true;
            break;
        case 1000: mMouseMode1000 = true; break;
        case 1002: mMouseMode1002 = true; break;
        case 1003: mMouseMode1003 = true; break;
        case 1006: mMouseMode1006 = true; break;
        case 1016: mMouseMode1016 = true; break;
        case 1049: // Alt screen: save cursor, switch to alt, clear
            mSavedCursorX = mCursorX;
            mSavedCursorY = mCursorY;
            mSavedAttrs = mCurrentAttrs;
            mSavedWrapPending = savedWrapPending;
            mUsingAltScreen = true;
            // Kitty: switch to alt screen's stack
            mKittyFlags = (mKittyStackDepthAlt > 0) ? mKittyStackAlt[mKittyStackDepthAlt - 1] : 0;
            notifyPointerShapeChanged();  // alt stack is now active
            mCursorX = 0;
            mCursorY = 0;
            for (int r = 0; r < mAltGrid.rows(); ++r) mAltGrid.clearRow(r);
            mAltGrid.markAllDirty();
            mScrollTop = 0;
            mScrollBottom = mHeight;
            clearSelection();
            break;
        case 1004: mFocusReporting = true; break;
        case 2004: mBracketedPaste = true; break;
        case 2026: mSyncOutput = true; break;
        case 2031: mColorPreferenceReporting = true; break;
        default:
            sLog().warn("Ignoring private mode set {}", action->count);
            break;
        }
        break;
    case Action::ResetMode:
        switch (action->count) {
        case 1: // DECCKM: normal cursor keys
            mCursorKeyMode = false;
            break;
        case 7: // DECAWM: no autowrap
            mAutoWrap = false;
            break;
        case 12: // ATT610: cursor blink off
            mCursorBlinkEnabled = false;
            break;
        case 25: // DECTCEM: hide cursor
            mCursorVisible = false;
            break;
        case 1000: mMouseMode1000 = false; break;
        case 1002: mMouseMode1002 = false; break;
        case 1003: mMouseMode1003 = false; break;
        case 1006: mMouseMode1006 = false; break;
        case 1016: mMouseMode1016 = false; break;
        case 1049: // Alt screen off: switch back to main, restore cursor
            mUsingAltScreen = false;
            // Kitty: switch back to main screen's stack
            mKittyFlags = (mKittyStackDepthMain > 0) ? mKittyStackMain[mKittyStackDepthMain - 1] : 0;
            notifyPointerShapeChanged();  // main stack is active again
            mCursorX = mSavedCursorX;
            mCursorY = mSavedCursorY;
            mCurrentAttrs = mSavedAttrs;
            mWrapPending = mSavedWrapPending;
            mDocument.markAllDirty();
            mScrollTop = 0;
            mScrollBottom = mHeight;
            clearSelection();
            break;
        case 1004: mFocusReporting = false; break;
        case 2004: mBracketedPaste = false; break;
        case 2026: mSyncOutput = false; break;
        case 2031: mColorPreferenceReporting = false; break;
        default:
            sLog().warn("Ignoring private mode reset {}", action->count);
            break;
        }
        break;
    default:
        break;
    }
}

const char *TerminalEmulator::Action::typeName(Type type)
{
    switch (type) {
    case Invalid: return "Invalid";
    case CursorUp: return "CursorUp";
    case CursorDown: return "CursorDown";
    case CursorForward: return "CursorForward";
    case CursorBack: return "CursorBack";
    case CursorNextLine: return "CursorNextLine";
    case CursorPreviousLine: return "CursorPreviousLine";
    case CursorHorizontalAbsolute: return "CursorHorizontalAbsolute";
    case CursorPosition: return "CursorPosition";
    case ClearScreen: return "ClearScreen";
    case ClearToBeginningOfScreen: return "ClearToBeginningOfScreen";
    case ClearToEndOfScreen: return "ClearToEndOfScreen";
    case ClearLine: return "ClearLine";
    case ClearToBeginningOfLine: return "ClearToBeginningOfLine";
    case ClearToEndOfLine: return "ClearToEndOfLine";
    case DeleteChars: return "DeleteChars";
    case InsertChars: return "InsertChars";
    case InsertLines: return "InsertLines";
    case DeleteLines: return "DeleteLines";
    case EraseChars: return "EraseChars";
    case ScrollUp: return "ScrollUp";
    case ScrollDown: return "ScrollDown";
    case VerticalPositionAbsolute: return "VerticalPositionAbsolute";
    case SelectGraphicRendition: return "SelectGraphicRendition";
    case AUXPortOn: return "AUXPortOn";
    case AUXPortOff: return "AUXPortOff";
    case DeviceStatusReport: return "DeviceStatusReport";
    case SaveCursorPosition: return "SaveCursorPosition";
    case RestoreCursorPosition: return "RestoreCursorPosition";
    case SetMode: return "SetMode";
    case ResetMode: return "ResetMode";
    }
    abort();
    return nullptr;
}

const char *TerminalEmulator::escapeSequenceName(EscapeSequence seq)
{
    switch (seq) {
    case SS2: return "SS2";
    case SS3: return "SS3";
    case DCS: return "DCS";
    case CSI: return "CSI";
    case ST: return "ST";
    case OSX: return "OSX";
    case SOS: return "SOS";
    case PM: return "PM";
    case APC: return "APC";
    case RIS: return "RIS";
    case VB: return "VB";
    case DECKPAM: return "DECKPAM";
    case DECKPNM: return "DECKPNM";
    case DECSC: return "DECSC";
    case DECRC: return "DECRC";
    case IND: return "IND";
    case NEL: return "NEL";
    case RI: return "RI";
    }
    abort();
    return nullptr;
}

const char *TerminalEmulator::csiSequenceName(CSISequence seq)
{
    switch (seq) {
    case CUU: return "CUU";
    case CUD: return "CUD";
    case CUF: return "CUF";
    case CUB: return "CUB";
    case CNL: return "CNL";
    case CPL: return "CPL";
    case CHA: return "CHA";
    case CUP: return "CUP";
    case ED: return "ED";
    case EL: return "EL";
    case SU: return "SU";
    case SD: return "SD";
    case HVP: return "HVP";
    case SGR: return "SGR";
    case REP: return "REP";
    case AUX: return "AUX";
    case DSR: return "DSR";
    case SCP: return "SCP";
    case IL: return "IL";
    case DL: return "DL";
    case ECH: return "ECH";
    case VPA: return "VPA";
    case RCP: return "RCP";
    case DCH: return "DCH";
    case ICH: return "ICH";
    case SM: return "SM";
    case RM: return "RM";
    case DECSTBM: return "DECSTBM";
    }
    return nullptr;
}
