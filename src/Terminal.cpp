#include "Terminal.h"
#include "Base64.h"
#include "Log.h"
#include "Utf8.h"
#include "Wcwidth.h"
#include <spdlog/spdlog.h>

#include <assert.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <limits>
#include <signal.h>
#include <algorithm>
#include <string_view>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

// Standard 16-color palette (standard 8 + bright 8)
const uint8_t Terminal::s16ColorPalette[16][3] = {
    {  0,   0,   0}, // 0 black
    {205,   0,   0}, // 1 red
    {  0, 205,   0}, // 2 green
    {205, 205,   0}, // 3 yellow
    {  0,   0, 238}, // 4 blue
    {205,   0, 205}, // 5 magenta
    {  0, 205, 205}, // 6 cyan
    {229, 229, 229}, // 7 white
    {127, 127, 127}, // 8 bright black (gray)
    {255,   0,   0}, // 9 bright red
    {  0, 255,   0}, // 10 bright green
    {255, 255,   0}, // 11 bright yellow
    { 92,  92, 255}, // 12 bright blue
    {255,   0, 255}, // 13 bright magenta
    {  0, 255, 255}, // 14 bright cyan
    {255, 255, 255}, // 15 bright white
};

void Terminal::color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (idx < 16) {
        r = s16ColorPalette[idx][0];
        g = s16ColorPalette[idx][1];
        b = s16ColorPalette[idx][2];
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

Terminal::Terminal(Platform *platform)
    : mPlatform(platform)
{
    memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
    memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
}

bool Terminal::init(const TerminalOptions &options)
{
    mOptions = options;
    // Re-initialize document with configured scrollback capacity
    mDocument = Document(mDocument.cols(), mDocument.rows(), mOptions.scrollbackLines);

    mMasterFD = posix_openpt(O_RDWR | O_NOCTTY);
    if (mMasterFD == -1) {
        FATAL("Failed to posix_openpt -> %d %s", errno, strerror(errno));
        return false;
    }

    if (grantpt(mMasterFD) == -1) {
        FATAL("Failed to grantpt -> %d %s", errno, strerror(errno));
        return false;
    }

    if (unlockpt(mMasterFD) == -1) {
        FATAL("Failed to unlockpt -> %d %s", errno, strerror(errno));
        return false;
    }

    char *slaveName = ptsname(mMasterFD);
    if (!slaveName) {
        FATAL("Failed to ptsname -> %d %s", errno, strerror(errno));
        return false;
    }

    int slaveFD;
    EINTRWRAP(slaveFD, open(slaveName, O_RDWR | O_NOCTTY));
    if (slaveFD == -1) {
        FATAL("Failed to open slave fd -> %d %s", errno, strerror(errno));
        return false;
    }

    const pid_t pid = fork();
    int ret;
    switch (pid) {
    case 0:
        EINTRWRAP(ret, ::close(mMasterFD));
        setsid();
        if (ioctl(slaveFD, TIOCSCTTY, NULL) == -1) {
            FATAL("Failed to ioctl slave fd in slave -> %d %s", errno, strerror(errno));
            return false;
        }

        for (int i=0; i<3; ++i) {
            EINTRWRAP(ret, dup2(slaveFD, i));
            if (ret == -1) {
                FATAL("Failed to dup2(%d) slave fd in slave -> %d %s", i, errno, strerror(errno));
                return false;
            }
        }

        EINTRWRAP(ret, ::close(slaveFD));

        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        setenv("LOGNAME", mOptions.user.c_str(), 1);
        setenv("USER", mOptions.user.c_str(), 1);
        setenv("SHELL", mOptions.shell.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);

        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);

        execl(mOptions.shell.c_str(), mOptions.shell.c_str(), nullptr);
        return false;

        break;
    case -1:
        FATAL("Failed to fork -> %d %s", errno, strerror(errno));
        return false;
    default:
        EINTRWRAP(ret, ::close(slaveFD));
        break;
    }
    return true;
}

Terminal::~Terminal()
{
    if (mMasterFD != -1) ::close(mMasterFD);
}

void Terminal::resize(int width, int height)
{
    mWidth = width;
    mHeight = height;
    int oldHistSize = mDocument.historySize();
    mDocument.resize(width, height);
    int histDelta = oldHistSize - mDocument.historySize();
    if (histDelta > 0) {
        // Rows pulled from history into screen — shift cursor down
        mCursorY += histDelta;
    } else if (histDelta < 0) {
        // Rows pushed from screen to history — shift cursor up
        mCursorY += histDelta;
    }
    mAltGrid.resize(width, height);
    mScrollTop = 0;
    mScrollBottom = height;
    mViewportOffset = std::clamp(mViewportOffset, 0, mDocument.historySize());
    mCursorX = std::min(mCursorX, width - 1);
    mCursorY = std::max(0, std::min(mCursorY, height - 1));
    clearSelection();
    event(Update);
}

void Terminal::scrollUpInRegion(int n)
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

const Cell* Terminal::viewportRow(int viewRow) const
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

void Terminal::scrollViewport(int delta)
{
    int oldOffset = mViewportOffset;
    // delta > 0 means scroll into history, delta < 0 toward live
    mViewportOffset = std::clamp(mViewportOffset + delta, 0, mDocument.historySize());
    if (mViewportOffset != oldOffset) {
        grid().markAllDirty();
        event(ScrollbackChanged);
    }
}

void Terminal::resetViewport()
{
    if (mViewportOffset != 0) {
        mViewportOffset = 0;
        grid().markAllDirty();
        event(ScrollbackChanged);
    }
}

void Terminal::advanceCursorToNewLine()
{
    // Line wrap: move to column 0 of next line, scrolling if needed
    mCursorX = 0;
    mCursorY++;
    if (mCursorY >= mScrollBottom) {
        mCursorY = mScrollBottom - 1;
        scrollUpInRegion(1);
    }
}

void Terminal::lineFeed()
{
    // LF: move cursor down one line, column unchanged. Scroll if at bottom of scroll region.
    mCursorY++;
    if (mCursorY >= mScrollBottom) {
        mCursorY = mScrollBottom - 1;
        scrollUpInRegion(1);
    }
}

void Terminal::keyPressEvent(const KeyEvent *event)
{
    resetViewport();

    spdlog::debug("keyPressEvent: key=0x{:x} text='{}' ({} bytes) count={}",
                  static_cast<int>(event->key),
                  toPrintable(event->text),
                  event->text.size(),
                  event->count);

    if (event->key == Key_F12) {
        return;
    }
    std::string text = event->text;
    if (text.empty()) {
        switch (event->key) {
        case Key_Return:
        case Key_Enter:    text = "\r"; break;
        case Key_Backspace: text = "\x7f"; break;
        case Key_Tab:      text = "\t"; break;
        case Key_Escape:   text = "\x1b"; break;
        case Key_Delete:   text = "\x1b[3~"; break;
        case Key_Left:     text = mCursorKeyMode ? "\x1bOD" : "\x1b[D"; break;
        case Key_Right:    text = mCursorKeyMode ? "\x1bOC" : "\x1b[C"; break;
        case Key_Up:       text = mCursorKeyMode ? "\x1bOA" : "\x1b[A"; break;
        case Key_Down:     text = mCursorKeyMode ? "\x1bOB" : "\x1b[B"; break;
        case Key_Home:     text = mCursorKeyMode ? "\x1bOH" : "\x1b[H"; break;
        case Key_End:      text = mCursorKeyMode ? "\x1bOF" : "\x1b[F"; break;
        case Key_PageUp:   text = "\x1b[5~"; break;
        case Key_PageDown: text = "\x1b[6~"; break;
        case Key_Insert:   text = "\x1b[2~"; break;
        case Key_F1:       text = "\x1bOP"; break;
        case Key_F2:       text = "\x1bOQ"; break;
        case Key_F3:       text = "\x1bOR"; break;
        case Key_F4:       text = "\x1bOS"; break;
        case Key_F5:       text = "\x1b[15~"; break;
        case Key_F6:       text = "\x1b[17~"; break;
        case Key_F7:       text = "\x1b[18~"; break;
        case Key_F8:       text = "\x1b[19~"; break;
        case Key_F9:       text = "\x1b[20~"; break;
        case Key_F10:      text = "\x1b[21~"; break;
        case Key_F11:      text = "\x1b[23~"; break;
        case Key_F12:      text = "\x1b[24~"; break;
        default:
            break;
        }
    }
    if (text.empty()) {
        spdlog::debug("keyPressEvent: no text to send for key=0x{:x}", static_cast<int>(event->key));
    }
    if (!text.empty() && event->count) {
        const char *ch = text.c_str();
        int bytes = text.size();
        spdlog::debug("keyPressEvent: writing {} bytes to masterFD={}", bytes, mMasterFD);
        for (size_t i=0; i<event->count; ++i) {
            int ret;
            while (bytes) {
                EINTRWRAP(ret, ::write(mMasterFD, ch, bytes));
                if (ret == -1) {
                    FATAL("Failed to write to master %d %s", errno, strerror(errno));
                    mPlatform->quit();
                    return;
                }
                assert(ret <= bytes);
                bytes -= ret;
                ch += ret;
            }
        }
    }
}

void Terminal::writeToPTY(const char* data, size_t len)
{
    while (len > 0) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, data, len));
        if (ret == -1) {
            FATAL("Failed to write to master %d %s", errno, strerror(errno));
            mPlatform->quit();
            return;
        }
        data += ret;
        len -= ret;
    }
}

void Terminal::pasteText(const std::string& text)
{
    if (mBracketedPaste) {
        writeToPTY("\x1b[200~", 6);
    }
    writeToPTY(text.c_str(), text.size());
    if (mBracketedPaste) {
        writeToPTY("\x1b[201~", 6);
    }
}

void Terminal::focusEvent(bool focused)
{
    if (mFocusReporting) {
        writeToPTY(focused ? "\x1b[I" : "\x1b[O", 3);
    }
}

void Terminal::sendMouseEvent(int button, bool press, bool motion, int cx, int cy, unsigned int modifiers)
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
        writeToPTY(buf, n);
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
        writeToPTY(buf, 6);
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

void Terminal::mousePressEvent(const MouseEvent *ev)
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

    // Begin selection
    clearSelection();
    int absRow = mDocument.historySize() - mViewportOffset + ev->y;
    startSelection(ev->x, absRow);
    event(Update);
}

void Terminal::mouseReleaseEvent(const MouseEvent *ev)
{
    if (mSelection.active) {
        finalizeSelection();
        std::string text = selectedText();
        if (!text.empty()) {
            copyToClipboard(text);
        }
        event(Update);
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

void Terminal::mouseMoveEvent(const MouseEvent *ev)
{
    if (mSelection.active) {
        int col = std::max(0, std::min(ev->x, mWidth - 1));
        int row = std::max(0, std::min(ev->y, mHeight - 1));
        int absRow = mDocument.historySize() - mViewportOffset + row;
        updateSelection(col, absRow);
        event(Update);
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
void Terminal::startSelection(int col, int absRow)
{
    mSelection.startCol = col;
    mSelection.startAbsRow = absRow;
    mSelection.endCol = col;
    mSelection.endAbsRow = absRow;
    mSelection.active = true;
    mSelection.valid = false;
}

void Terminal::updateSelection(int col, int absRow)
{
    mSelection.endCol = col;
    mSelection.endAbsRow = absRow;
}

void Terminal::finalizeSelection()
{
    mSelection.active = false;
    mSelection.valid = true;
}

void Terminal::clearSelection()
{
    mSelection.active = false;
    mSelection.valid = false;
}

bool Terminal::isCellSelected(int col, int absRow) const
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


std::string Terminal::selectedText() const
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

void Terminal::readFromFD()
{
    char buf[4096];
#ifndef NDEBUG
    memset(buf, 0, sizeof(buf));
#endif
    int ret;
    EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf) - 1));
    spdlog::debug("readFromFD: read {} bytes from masterFD={}", ret, mMasterFD);
    if (ret == -1) {
        ERROR("Failed to read from mMasterFD %d %s", errno, strerror(errno));
        mPlatform->quit();
        return;
    } else if (ret == 0) {
        mPlatform->quit();
        return;
    }
    DEBUG("readFromFD: \"%s\"", toPrintable(buf, ret).c_str());
    const int len = ret;

    auto resetToNormal = [this]() {
        assert(mState == InEscape || mState == InStringSequence);
        mState = Normal;
        mEscapeIndex = 0;
#ifndef NDEBUG
        memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
#endif
    };

    IGrid& g = grid();

    for (int i=0; i<len; ++i) {
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
            case '\a':
                break;
            default:
                if (static_cast<unsigned char>(buf[i]) >= 0x80) {
                    assert(mUtf8Index == 0);
                    mUtf8Buffer[mUtf8Index++] = buf[i];
                    mState = InUtf8;
                } else {
                    // ASCII character — write to cell grid
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        g.cell(mCursorX, mCursorY) = Cell{static_cast<char32_t>(buf[i]), mCurrentAttrs};
                        g.clearExtra(mCursorX, mCursorY);
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
                    if (mWrapPending) {
                        advanceCursorToNewLine();
                        mWrapPending = false;
                    }
                    if (mCursorX >= 0 && mCursorX < mWidth && mCursorY >= 0 && mCursorY < mHeight) {
                        g.cell(mCursorX, mCursorY) = Cell{cp, mCurrentAttrs};
                        g.clearExtra(mCursorX, mCursorY);
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
            assert(mEscapeIndex < static_cast<int>(sizeof(mEscapeBuffer)));
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
                    } else if (mEscapeIndex == sizeof(mEscapeBuffer)) {
                        ERROR("CSI sequence is too long %zu", sizeof(mEscapeBuffer));
                        resetToNormal();
                    } else if (buf[i] < 0x20 || buf[i] > 0x3f) {
                        ERROR("Invalid CSI sequence 0x%0x character", buf[i]);
                        resetToNormal();
                    }
                }
                break;
            case RIS:
                // Reset to initial state
                mWrapPending = false;
                mCurrentAttrs.reset();
                mCursorX = 0;
                mCursorY = 0;
                mScrollTop = 0;
                mScrollBottom = mHeight;
                g.markAllDirty();
                for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
                resetToNormal();
                break;
            case VB:
                event(VisibleBell);
                resetToNormal();
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

    event(Update);
}

void Terminal::processCSI()
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

    // Check for prefix characters '?' and '>'
    bool isPrivate = (mEscapeIndex > 1 && mEscapeBuffer[1] == '?');
    bool isSecondary = (mEscapeIndex > 1 && mEscapeBuffer[1] == '>');

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
        if (mEscapeIndex == 3 && mEscapeBuffer[1] == '6') {
            // Report cursor position: ESC [ row ; col R
            char response[32];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;%dR",
                                mCursorY + 1, mCursorX + 1);
            int written;
            EINTRWRAP(written, ::write(mMasterFD, response, rlen));
        } else {
            ERROR("Invalid DSR command (%d)", mEscapeIndex);
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
        if (isPrivate || isSecondary) {
            // CSI ? u — kitty keyboard query; CSI > Ps u — push keyboard mode
            WARN("Ignoring kitty keyboard protocol: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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
            writeToPTY("\x1b[>64;2500;0c", 13);
        } else if (mEscapeIndex == 2 || (mEscapeIndex == 3 && mEscapeBuffer[1] == '0')) {
            // CSI c or CSI 0 c — Primary DA: VT420 with common features
            writeToPTY("\x1b[?64;1;2;6;22c", 16);
        } else {
            WARN("Ignoring DA variant: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
        }
        break;
    case 'q':
        if (isSecondary) {
            // CSI > q — XTVERSION: report terminal name/version
            static const char xtver[] = "\x1bP>|MasterBandit(0.1)\x1b\\";
            writeToPTY(xtver, sizeof(xtver) - 1);
        } else {
            // CSI Ps SP q — DECSCUSR (Set Cursor Style): ignore for now
            WARN("Ignoring DECSCUSR / CSI q: %s", toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
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

void Terminal::onAction(const Action *action)
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
        default:
            WARN("Ignoring private mode reset %d", action->count);
            break;
        }
        break;
    default:
        break;
    }
}

void Terminal::processSGR()
{
    assert(mEscapeBuffer[0] == CSI);
    assert(mEscapeBuffer[mEscapeIndex - 1] == 'm');

    // Parse semicolon-delimited parameters from mEscapeBuffer[1..mEscapeIndex-2]
    // e.g. "[0;31m" -> params = {0, 31}
    std::vector<int> params;
    {
        const char* start = mEscapeBuffer + 1;
        const char* end = mEscapeBuffer + mEscapeIndex - 1;
        if (start == end) {
            params.push_back(0); // bare ESC[m = reset
        } else {
            while (start < end) {
                char* next;
                long val = strtol(start, &next, 10);
                params.push_back(static_cast<int>(val));
                if (next < end && *next == ';') {
                    start = next + 1;
                } else {
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int p = params[i];

        switch (p) {
        case 0: // Reset
            mCurrentAttrs.reset();
            break;
        case 1: mCurrentAttrs.setBold(true); break;
        case 2: mCurrentAttrs.setDim(true); break;
        case 3: mCurrentAttrs.setItalic(true); break;
        case 4: mCurrentAttrs.setUnderline(true); break;
        case 5: mCurrentAttrs.setBlink(true); break;
        case 7: mCurrentAttrs.setInverse(true); break;
        case 8: mCurrentAttrs.setInvisible(true); break;
        case 9: mCurrentAttrs.setStrikethrough(true); break;

        case 21: // doubly underlined or bold off (varies)
        case 22: mCurrentAttrs.setBold(false); mCurrentAttrs.setDim(false); break;
        case 23: mCurrentAttrs.setItalic(false); break;
        case 24: mCurrentAttrs.setUnderline(false); break;
        case 25: mCurrentAttrs.setBlink(false); break;
        case 27: mCurrentAttrs.setInverse(false); break;
        case 28: mCurrentAttrs.setInvisible(false); break;
        case 29: mCurrentAttrs.setStrikethrough(false); break;

        // Foreground standard colors (30-37)
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37: {
            int idx = p - 30;
            mCurrentAttrs.setFg(s16ColorPalette[idx][0], s16ColorPalette[idx][1], s16ColorPalette[idx][2]);
            mCurrentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        case 38: // Extended foreground
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    // 256-color: 38;5;N
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2], r, g, b);
                    mCurrentAttrs.setFg(r, g, b);
                    mCurrentAttrs.setFgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    // Truecolor: 38;2;R;G;B
                    mCurrentAttrs.setFg(
                        static_cast<uint8_t>(params[i + 2]),
                        static_cast<uint8_t>(params[i + 3]),
                        static_cast<uint8_t>(params[i + 4]));
                    mCurrentAttrs.setFgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 39: // Default foreground
            mCurrentAttrs.setFgMode(CellAttrs::Default);
            break;

        // Background standard colors (40-47)
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47: {
            int idx = p - 40;
            mCurrentAttrs.setBg(s16ColorPalette[idx][0], s16ColorPalette[idx][1], s16ColorPalette[idx][2]);
            mCurrentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        case 48: // Extended background
            if (i + 1 < params.size()) {
                if (params[i + 1] == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2], r, g, b);
                    mCurrentAttrs.setBg(r, g, b);
                    mCurrentAttrs.setBgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                    mCurrentAttrs.setBg(
                        static_cast<uint8_t>(params[i + 2]),
                        static_cast<uint8_t>(params[i + 3]),
                        static_cast<uint8_t>(params[i + 4]));
                    mCurrentAttrs.setBgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 49: // Default background
            mCurrentAttrs.setBgMode(CellAttrs::Default);
            break;

        // Bright foreground (90-97)
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97: {
            int idx = p - 90 + 8;
            mCurrentAttrs.setFg(s16ColorPalette[idx][0], s16ColorPalette[idx][1], s16ColorPalette[idx][2]);
            mCurrentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        // Bright background (100-107)
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107: {
            int idx = p - 100 + 8;
            mCurrentAttrs.setBg(s16ColorPalette[idx][0], s16ColorPalette[idx][1], s16ColorPalette[idx][2]);
            mCurrentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        default:
            DEBUG("Unhandled SGR param %d", p);
            break;
        }
    }
}

static inline bool gettime(timeval *time)
{
#if defined(__APPLE__)
    static mach_timebase_info_data_t info;
    static bool first = true;
    unsigned long long machtime = mach_absolute_time();
    if (first) {
        first = false;
        mach_timebase_info(&info);
    }
    machtime = machtime * info.numer / (info.denom * 1000);
    time->tv_sec = machtime / 1000000;
    time->tv_usec = machtime % 1000000;
#elif defined(__linux__)
    timespec spec;
    const clockid_t cid = CLOCK_MONOTONIC_RAW;
    const int ret = ::clock_gettime(cid, &spec);
    if (ret == -1) {
        memset(time, 0, sizeof(timeval));
        return false;
    }
    time->tv_sec = spec.tv_sec;
    time->tv_usec = spec.tv_nsec / 1000;
#else
#error No gettime() implementation
#endif
    return true;
}


// --- OSC 1337 iTerm2 inline images ---
void Terminal::processOSC_iTerm(std::string_view payload)
{
    // Format: "File=[params]:base64data"
    if (payload.substr(0, 5) != "File=") return;

    size_t colonPos = payload.find(':');
    if (colonPos == std::string_view::npos) return;

    std::string_view paramStr = payload.substr(5, colonPos - 5);
    std::string_view b64data = payload.substr(colonPos + 1);

    // Parse params
    bool isInline = false;
    std::string_view::size_type pos = 0;
    while (pos < paramStr.size()) {
        auto eq = paramStr.find('=', pos);
        if (eq == std::string_view::npos) break;
        auto semi = paramStr.find(';', eq);
        if (semi == std::string_view::npos) semi = paramStr.size();

        std::string_view key = paramStr.substr(pos, eq - pos);
        std::string_view val = paramStr.substr(eq + 1, semi - eq - 1);

        if (key == "inline" && val == "1") isInline = true;
        pos = semi + 1;
    }

    if (!isInline) return;

    // Decode base64
    std::vector<uint8_t> imageBytes = base64::decode(b64data);
    if (imageBytes.empty()) return;

    // Decode image
    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(
        imageBytes.data(), static_cast<int>(imageBytes.size()), &w, &h, &channels, 4);
    if (!pixels) {
        DEBUG("OSC 1337: stbi_load_from_memory failed");
        return;
    }

    float cw = cellPixelWidth();
    float ch = cellPixelHeight();
    if (cw <= 0 || ch <= 0) {
        stbi_image_free(pixels);
        return;
    }

    int cellCols = std::max(1, static_cast<int>(std::ceil(static_cast<float>(w) / cw)));
    int cellRows = std::max(1, static_cast<int>(std::ceil(static_cast<float>(h) / ch)));

    ImageEntry entry;
    entry.id = mNextImageId++;
    entry.pixelWidth = w;
    entry.pixelHeight = h;
    entry.cellWidth = cellCols;
    entry.cellHeight = cellRows;
    entry.rgba.assign(pixels, pixels + w * h * 4);
    stbi_image_free(pixels);

    uint32_t imageId = entry.id;
    spdlog::warn("OSC 1337: image id={} {}x{} px, {}x{} cells",
                 imageId, w, h, cellCols, cellRows);
    mImageRegistry[imageId] = std::move(entry);

    placeImageInGrid(imageId, cellCols, cellRows);
}

void Terminal::placeImageInGrid(uint32_t imageId, int cellCols, int cellRows)
{
    IGrid& g = grid();
    int fillCols = std::min(cellCols, mWidth);

    for (int r = 0; r < cellRows; ++r) {
        // Scroll if cursor is at the bottom
        if (mCursorY >= mHeight) {
            scrollUpInRegion(1);
            mCursorY = mHeight - 1;
        }

        // Fill cells with blanks to reserve visual space
        for (int c = 0; c < fillCols; ++c) {
            Cell& cell = g.cell(c, mCursorY);
            cell.wc = 0;
            cell.attrs.setWideSpacer(true);
        }

        // Place one extra at column 0 with image ID and row offset
        CellExtra& ex = g.ensureExtra(0, mCursorY);
        ex.imageId = imageId;
        ex.imageOffsetCol = 0;
        ex.imageOffsetRow = r;

        g.markRowDirty(mCursorY);
        mCursorY++;
    }

    if (mCursorY >= mHeight) {
        mCursorY = mHeight - 1;
    }
    mCursorX = 0;
}

// --- OSC processing ---
void Terminal::processStringSequence()
{
    if (mStringSequenceType != OSX) {
        WARN("Ignoring non-OSC string sequence type 0x%x (len=%zu)", mStringSequenceType, mStringSequence.size());
        return;
    }

    // mStringSequence format: "<number>;<payload>" or just "<number>"
    size_t semi = mStringSequence.find(';');
    if (semi == std::string::npos) return;

    int oscNum = 0;
    for (size_t i = 0; i < semi; ++i) {
        char c = mStringSequence[i];
        if (c < '0' || c > '9') return;
        oscNum = oscNum * 10 + (c - '0');
    }
    std::string_view payload(mStringSequence.data() + semi + 1, mStringSequence.size() - semi - 1);

    switch (oscNum) {
    case 0: processOSC_Title(payload, true); break;
    case 1: break; // icon name only — not used
    case 2: processOSC_Title(payload, true); break;
    case 52: processOSC_Clipboard(payload); break;
    case 1337: processOSC_iTerm(payload); break;
    default:
        WARN("Ignoring OSC %d", oscNum);
        break;
    }
}

void Terminal::processOSC_Title(std::string_view text, bool setTitle)
{
    if (setTitle) {
        onTitleChanged(std::string(text));
    }
}

void Terminal::processOSC_Clipboard(std::string_view payload)
{
    // Format: "c;base64data" or "c;?"
    size_t semi = payload.find(';');
    if (semi == std::string_view::npos) return;

    std::string_view data = payload.substr(semi + 1);
    if (data == "?") {
        // Query clipboard
        std::string clip = pasteFromClipboard();
        std::string encoded = base64::encode(
            reinterpret_cast<const uint8_t*>(clip.data()), clip.size());
        std::string response = "\x1b]52;c;" + encoded + "\x1b\\";
        writeToPTY(response.data(), response.size());
    } else {
        // Set clipboard
        std::vector<uint8_t> decoded = base64::decode(data);
        std::string text(decoded.begin(), decoded.end());
        copyToClipboard(text);
    }
}

unsigned long long Terminal::mono()
{
    timeval time;
    if (gettime(&time)) {
        return (time.tv_sec * static_cast<uint64_t>(1000)) + (time.tv_usec / static_cast<uint64_t>(1000));
    }
    return 0;
}

const char *Terminal::Action::typeName(Type type)
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

const char *Terminal::escapeSequenceName(EscapeSequence seq)
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

const char *Terminal::csiSequenceName(CSISequence seq)
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

