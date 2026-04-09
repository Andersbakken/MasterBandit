#include "TerminalEmulator.h"
#include "Config.h"
#include "Utils.h"
#include "Utf8.h"
#include "Wcwidth.h"
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
}

void TerminalEmulator::resize(int width, int height)
{
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

const Cell* TerminalEmulator::viewportRow(int viewRow) const
{
    if (mViewportOffset == 0) {
        return grid().row(viewRow);
    }
    int histSize = mDocument.historySize();
    int logicalRow = histSize - mViewportOffset + viewRow;
    if (logicalRow < histSize) {
        return mDocument.historyRow(logicalRow);
    } else {
        return grid().row(logicalRow - histSize);
    }
}

void TerminalEmulator::scrollViewport(int delta)
{
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
    if (mViewportOffset != 0) {
        mViewportOffset = 0;
        grid().markAllDirty();
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
    }
}

void TerminalEmulator::scrollToPrompt(int direction)
{
    // Scan history + screen rows for PromptStart markers.
    // Rows are indexed as absolute: 0 = oldest history, historySize() + screenRow = screen.
    // Current viewport top is at absolute row: historySize() - mViewportOffset.
    int histSize = mDocument.historySize();
    int totalRows = histSize + mHeight;

    // Current position: the row at the top of the viewport
    int currentAbsRow = histSize - mViewportOffset;

    if (direction < 0) {
        // Search upward from current viewport top - 1
        for (int absRow = currentAbsRow - 1; absRow >= 0; --absRow) {
            Document::PromptKind pk;
            if (absRow < histSize) {
                pk = mDocument.historyRowPromptKind(absRow);
            } else {
                pk = mDocument.rowPromptKind(absRow - histSize);
            }
            if (pk == Document::PromptStart) {
                // Scroll so this row is near the top of the viewport
                int newOffset = histSize - absRow;
                mViewportOffset = std::clamp(newOffset, 0, histSize);
                grid().markAllDirty();
                if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
                return;
            }
        }
    } else {
        // Search downward from current viewport top + 1
        for (int absRow = currentAbsRow + 1; absRow < totalRows; ++absRow) {
            Document::PromptKind pk;
            if (absRow < histSize) {
                pk = mDocument.historyRowPromptKind(absRow);
            } else {
                pk = mDocument.rowPromptKind(absRow - histSize);
            }
            if (pk == Document::PromptStart) {
                int newOffset = histSize - absRow;
                if (newOffset <= 0) {
                    // On screen or past it — reset to live
                    resetViewport();
                } else {
                    mViewportOffset = std::clamp(newOffset, 0, histSize);
                    grid().markAllDirty();
                    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
                }
                return;
            }
        }
        // No next prompt found — jump to live
        resetViewport();
    }
}

void TerminalEmulator::selectCommandOutput()
{
    // Find the command output region around the current viewport position.
    // Look for OutputStart (C) above, then PromptStart (A) below for the boundary.
    int histSize = mDocument.historySize();
    int currentAbsRow = histSize - mViewportOffset + (mHeight / 2); // middle of viewport

    // Find OutputStart at or above current position
    int outputStart = -1;
    for (int absRow = currentAbsRow; absRow >= 0; --absRow) {
        Document::PromptKind pk;
        if (absRow < histSize) pk = mDocument.historyRowPromptKind(absRow);
        else pk = mDocument.rowPromptKind(absRow - histSize);

        if (pk == Document::OutputStart) { outputStart = absRow; break; }
        if (pk == Document::PromptStart && absRow < currentAbsRow) break; // passed a prompt boundary
    }
    if (outputStart < 0) return;

    // Find end: next PromptStart or end of content
    int outputEnd = histSize + mHeight - 1;
    for (int absRow = outputStart + 1; absRow < histSize + mHeight; ++absRow) {
        Document::PromptKind pk;
        if (absRow < histSize) pk = mDocument.historyRowPromptKind(absRow);
        else pk = mDocument.rowPromptKind(absRow - histSize);

        if (pk == Document::PromptStart) { outputEnd = absRow - 1; break; }
    }

    // Select the range
    startSelection(0, outputStart);
    updateSelection(mWidth - 1, outputEnd);
    finalizeSelection();

    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard) {
        mCallbacks.copyToClipboard(text);
    }
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
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
    sLog().debug("injectData: \"{}\"", toPrintable(buf, static_cast<int>(len_)));
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
                        mWrapPending = true;
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
                                mWrapPending = true;
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
                        mWrapPending = true;
                    }
                } else {
                    // Normal single-width character
                    mLastPrintedChar = cp;
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
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
                        mWrapPending = true;
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
                mCursorShape = CursorBlock;
                mScrollTop = 0;
                mScrollBottom = mHeight;
                mCursorKeyMode = false;
                mKeypadMode = false;
                mMouseMode1000 = false;
                mMouseMode1002 = false;
                mMouseMode1003 = false;
                mMouseMode1006 = false;
                mBracketedPaste = false;
                mFocusReporting = false;
                mSyncOutput = false;
                mColorPreferenceReporting = false;
                if (mUsingAltScreen) {
                    mUsingAltScreen = false;
                    mDocument.markAllDirty();
                }
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
                mStringSequence += buf[i];
            }
            break;
        }
    }

    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::processCSI()
{
    assert(mState == InEscape);
    assert(mEscapeIndex >= 1);

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
                    mWrapPending = true;
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
            // CSI ? Ps s — XTERM save private mode values (ignore)
            sLog().warn("Ignoring XTERM save private mode: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
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
            sLog().warn("Ignoring non-private SM: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case RM: // Reset Mode
        if (isPrivate) {
            action.type = Action::ResetMode;
            char *end;
            action.count = strtoul(mEscapeBuffer + 2, &end, 10);
        } else {
            sLog().warn("Ignoring non-private RM: {}", toPrintable(mEscapeBuffer, mEscapeIndex));
        }
        break;
    case DECSTBM: { // Set scrolling region
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
    default:
        sLog().error("Unknown CSI final byte {:#x}", static_cast<unsigned char>(finalByte));
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

void TerminalEmulator::onAction(const Action *action)
{
    assert(action);
    assert(action->type != Action::Invalid);
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
        case 25: // DECTCEM: show cursor
            mCursorVisible = true;
            break;
        case 1000: mMouseMode1000 = true; break;
        case 1002: mMouseMode1002 = true; break;
        case 1003: mMouseMode1003 = true; break;
        case 1006: mMouseMode1006 = true; break;
        case 1049: // Alt screen: save cursor, switch to alt, clear
            mSavedCursorX = mCursorX;
            mSavedCursorY = mCursorY;
            mSavedAttrs = mCurrentAttrs;
            mSavedWrapPending = savedWrapPending;
            mUsingAltScreen = true;
            // Kitty: switch to alt screen's stack
            mKittyFlags = (mKittyStackDepthAlt > 0) ? mKittyStackAlt[mKittyStackDepthAlt - 1] : 0;
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
        case 25: // DECTCEM: hide cursor
            mCursorVisible = false;
            break;
        case 1000: mMouseMode1000 = false; break;
        case 1002: mMouseMode1002 = false; break;
        case 1003: mMouseMode1003 = false; break;
        case 1006: mMouseMode1006 = false; break;
        case 1049: // Alt screen off: switch back to main, restore cursor
            mUsingAltScreen = false;
            // Kitty: switch back to main screen's stack
            mKittyFlags = (mKittyStackDepthMain > 0) ? mKittyStackMain[mKittyStackDepthMain - 1] : 0;
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
