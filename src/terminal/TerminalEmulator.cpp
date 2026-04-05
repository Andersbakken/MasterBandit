#include "TerminalEmulator.h"
#include "Config.h"
#include "Utils.h"
#include "Log.h"
#include "Utf8.h"
#include "Wcwidth.h"
#include <spdlog/spdlog.h>

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
        // Count non-empty content lines before reflow (like kitty's
        // num_content_lines_before). Cursor beyond this = at a prompt.
        int contentLinesBefore = 0;
        if (mDocument.rows() > 0) {
            for (int r = mDocument.rows() - 1; r >= 0; --r) {
                const Cell* row = mDocument.row(r);
                bool empty = true;
                for (int c = 0; c < oldCols && empty; ++c) {
                    if (row[c].wc != 0) empty = false;
                }
                if (!empty) { contentLinesBefore = r + 1; break; }
            }
        }
        bool cursorBeyondContent = mCursorY >= contentLinesBefore;
        int linesAfterCursor = mHeight - mCursorY;

        int oldHistSize = mDocument.historySize();
        Document::CursorTrack ct;
        ct.srcX = mCursorX;
        ct.srcY = oldHistSize + mCursorY;
        ct.dstX = 0;
        ct.dstY = 0;

        mDocument.resize(width, height, &ct);

        int newHistSize = mDocument.historySize();

        // Count content lines after reflow
        int contentLinesAfter = 0;
        if (mDocument.rows() > 0) {
            for (int r = mDocument.rows() - 1; r >= 0; --r) {
                const Cell* row = mDocument.row(r);
                bool empty = true;
                for (int c = 0; c < width && empty; ++c) {
                    if (row[c].wc != 0) empty = false;
                }
                if (!empty) { contentLinesAfter = r + 1; break; }
            }
        }

        if (cursorBeyondContent) {
            // Cursor was at/past end of content (e.g. at a prompt with rprompt).
            // Place cursor at end of content, not where reflow tracked it.
            // This prevents rprompt wrapping from pushing the cursor down.
            mCursorX = std::min(ct.dstX, width - 1);
            mCursorY = std::min(contentLinesAfter, height - 1);
        } else {
            mCursorX = std::min(ct.dstX, width - 1);
            mCursorY = std::max(0, std::min(ct.dstY - newHistSize, height - 1));
        }
    } else {
        int oldHistSize = mDocument.historySize();
        mDocument.resize(width, height);
        if (oldCols == width) {
            // Height-only change: adjust cursor for history push/pull
            int histDelta = oldHistSize - mDocument.historySize();
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
    DEBUG("injectData: \"%s\"", toPrintable(buf, static_cast<int>(len_)).c_str());
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
                ERROR("Bad utf8 sequence (non-continuation byte)");
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mState = Normal;
                --i; // reprocess
                break;
            }
            mUtf8Buffer[mUtf8Index++] = buf[i];

            int expected = 0;
            unsigned char lead = static_cast<unsigned char>(mUtf8Buffer[0]);
            if ((lead & 0xE0) == 0xC0) expected = 2;
            else if ((lead & 0xF0) == 0xE0) expected = 3;
            else if ((lead & 0xF8) == 0xF0) expected = 4;
            else expected = 1;

            if (mUtf8Index == expected) {
                // Decode the codepoint
                char32_t cp = 0;
                if (expected == 2) {
                    cp = (static_cast<unsigned char>(mUtf8Buffer[0]) & 0x1F) << 6
                       | (static_cast<unsigned char>(mUtf8Buffer[1]) & 0x3F);
                } else if (expected == 3) {
                    cp = (static_cast<unsigned char>(mUtf8Buffer[0]) & 0x0F) << 12
                       | (static_cast<unsigned char>(mUtf8Buffer[1]) & 0x3F) << 6
                       | (static_cast<unsigned char>(mUtf8Buffer[2]) & 0x3F);
                } else if (expected == 4) {
                    cp = (static_cast<unsigned char>(mUtf8Buffer[0]) & 0x07) << 18
                       | (static_cast<unsigned char>(mUtf8Buffer[1]) & 0x3F) << 12
                       | (static_cast<unsigned char>(mUtf8Buffer[2]) & 0x3F) << 6
                       | (static_cast<unsigned char>(mUtf8Buffer[3]) & 0x3F);
                }

                int w = wcwidth(cp);
                if (w < 0) w = 0; // non-printable control char — skip
                if (w == 0) {
                    // Zero-width / combining character — attach to previous cell
                    // (for now, just skip it)
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
                ERROR("Bad utf8 (overlong sequence)");
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
                ERROR("Escape buffer overflow");
                resetToNormal();
                break;
            }
            mEscapeBuffer[mEscapeIndex++] = buf[i];
            DEBUG("Adding escape byte [%d] %s %d -> %s", mEscapeIndex - 1,
                  toPrintable(buf + i, 1).c_str(),
                  i, toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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
                        ERROR("CSI sequence is too long %zu", sizeof(mEscapeBuffer));
                        resetToNormal();
                    } else if (buf[i] < 0x20 || buf[i] > 0x3f) {
                        ERROR("Invalid CSI sequence 0x%0x character", buf[i]);
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
                ERROR("Unknown escape sequence %s\n", toPrintable(mEscapeBuffer, 1).c_str());
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

    DEBUG("Processing CSI \"%s\"", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());

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
            ERROR("Invalid parameters for CSI %s", csiSequenceName(static_cast<CSISequence>(finalByte)));
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
                    ERROR("Invalid CSI CUP error 1");
                    action.type = Action::Invalid;
                } else {
                    action.x = x;
                }
            }
        } else if (*end == ';') {
            if (mEscapeBuffer[1] != ';') {
                if (x == 0) {
                    ERROR("Invalid CSI CUP error 2");
                    action.type = Action::Invalid;
                    break;
                }
                action.x = x;
            }
            if (end + 1 < mEscapeBuffer + mEscapeIndex - 1) {
                const unsigned long y = strtoul(end + 1, &end, 10);
                if (end != mEscapeBuffer + mEscapeIndex - 1 || !y) {
                    ERROR("Invalid CSI CUP error 3");
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
        default: ERROR("Invalid CSI ED %d", readCount(0)); break;
        }
        break;
    case EL:
        switch (readCount(0)) {
        case 0: action.type = Action::ClearToEndOfLine; break;
        case 1: action.type = Action::ClearToBeginningOfLine; break;
        case 2: action.type = Action::ClearLine; break;
        default: ERROR("Invalid CSI EL %d", readCount(0)); break;
        }
        break;
    case SU: {
        const int n = readCount(1);
        if (n <= 0) {
            ERROR("Invalid CSI SU %d", n);
        } else {
            action.type = Action::ScrollUp;
            action.count = n;
        }
        break; }
    case SD: {
        const int n = readCount(1);
        if (n <= 0) {
            ERROR("Invalid CSI SD %d", n);
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
            ERROR("Invalid AUX CSI command (%d)", mEscapeIndex);
        } else if (mEscapeBuffer[1] == '4') {
            action.type = Action::AUXPortOff;
        } else if (mEscapeBuffer[1] == '5') {
            action.type = Action::AUXPortOn;
        } else {
            ERROR("Invalid AUX CSI command (0x%0x)", mEscapeBuffer[1]);
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
                WARN("Unhandled private DSR %d", ps);
            }
        } else if (mEscapeIndex == 3 && mEscapeBuffer[1] == '6') {
            // Report cursor position: ESC [ row ; col R
            char response[32];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;%dR",
                                mCursorY + 1, mCursorX + 1);
            writeToOutput(response, rlen);
        } else {
            WARN("Unhandled DSR: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
        }
        break;
    case SCP:
        if (isPrivate) {
            // CSI ? Ps s — XTERM save private mode values (ignore)
            WARN("Ignoring XTERM save private mode: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
        } else if (mEscapeIndex == 2) {
            action.type = Action::SaveCursorPosition;
        } else {
            WARN("Ignoring CSI s variant: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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
            WARN("Ignoring CSI u variant: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
        }
        break;
    case SM: // Set Mode
        if (isPrivate) {
            action.type = Action::SetMode;
            // Parse the mode number
            char *end;
            action.count = strtoul(mEscapeBuffer + 2, &end, 10); // skip "[?"
        } else {
            WARN("Ignoring non-private SM: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
        }
        break;
    case RM: // Reset Mode
        if (isPrivate) {
            action.type = Action::ResetMode;
            char *end;
            action.count = strtoul(mEscapeBuffer + 2, &end, 10);
        } else {
            WARN("Ignoring non-private RM: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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
            WARN("Ignoring DA variant: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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
    default:
        ERROR("Unknown CSI final byte 0x%x", finalByte);
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
    DEBUG("Got action %s %d %d %d", Action::typeName(action->type), action->count, action->x, action->y);

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
            WARN("Ignoring private mode set %d", action->count);
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
            WARN("Ignoring private mode reset %d", action->count);
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
