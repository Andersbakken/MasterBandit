#include "Terminal.h"
#include "Log.h"

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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

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
    mLines.push_back(Line());
    memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
    memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
}

bool Terminal::init(const TerminalOptions &options)
{
// #warning should look at all that st does here, set some environment variables, handle some signals, etc
    mOptions = options;
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

    EINTRWRAP(mSlaveFD, open(slaveName, O_RDWR | O_NOCTTY));
    if (mSlaveFD == -1) {
        FATAL("Failed to open slave fd -> %d %s", errno, strerror(errno));
        return false;
    }

    const pid_t pid = fork();
    int ret;
    switch (pid) {
    case 0:
        EINTRWRAP(ret, ::close(mMasterFD));
        setsid();
        if (ioctl(mSlaveFD, TIOCSCTTY, NULL) == -1) {
            FATAL("Failed to ioctl slave fd in slave -> %d %s", errno, strerror(errno));
            return false;
        }

        for (int i=0; i<3; ++i) {
            EINTRWRAP(ret, dup2(mSlaveFD, i));
            if (ret == -1) {
                FATAL("Failed to dup2(%d) slave fd in slave -> %d %s", i, errno, strerror(errno));
                return false;
            }
        }

        EINTRWRAP(ret, ::close(mSlaveFD));

        extern char **environ;
        execle(mOptions.shell.c_str(), mOptions.shell.c_str(), nullptr, environ);

        // const struct passwd *pw;

        // unsetenv("COLUMNS");
        // unsetenv("LINES");
        // unsetenv("TERMCAP");
        // setenv("LOGNAME", options.user.c_str(), 1);
        // setenv("USER", options.user.c_str(), 1);
        // setenv("SHELL", options.shell.c_str(), 1);
        // // setenv("HOME", gete, 1);
        // setenv("TERM", "xterm-256color", 1);

        // signal(SIGCHLD, SIG_DFL);
        // signal(SIGHUP, SIG_DFL);
        // signal(SIGINT, SIG_DFL);
        // signal(SIGQUIT, SIG_DFL);
        // signal(SIGTERM, SIG_DFL);
        // signal(SIGALRM, SIG_DFL);

        // execl(mOptions.shell.c_str(), mOptions.shell.c_str(), nullptr);
        return false;

        break;
    case -1:
        FATAL("Failed to fork -> %d %s", errno, strerror(errno));
        return false;
    default:
        EINTRWRAP(ret, ::close(mSlaveFD));
        break;
    }
    return true;
}

Terminal::~Terminal()
{
}

void Terminal::scroll(int x, int y)
{
    mX = x;
    mY = y;
    event(Update);
}

void Terminal::resize(int width, int height)
{
    mWidth = width;
    mHeight = height;
    event(Update);
}

void Terminal::render()
{
    const int max = std::min<int>(mY + mHeight, mLines.size());
    VERBOSE("RENDER %zu mHeight %d mY %d max %d\n", mLines.size(), mHeight, mY, max);
    for (int y = mY; y<max; ++y) {
        const Line &line = mLines[y];
        const bool c = y == mCursorY;
        VERBOSE("RENDERING %d %s %d %d\n", y, toPrintable(line.data).c_str(), c, mCursorX);
        int start, length;
        if (isSelected(y, &start, &length)) {
            assert(start + length <= line.data.size());
            if (start > 0) {
                render(0, y, line.data, 0, start, c && mCursorX < start ? mCursorX : -1, Render_None);
            }
            render(start, y, line.data, start, length,
                   c && mCursorX >= start && mCursorX < start + length ? mCursorX : -1,
                   Render_Selected);
            if (start + length < line.data.size())
                render(start + length, y, line.data, start + length, line.data.size() - start - length,
                       c && mCursorX >= start + length ? mCursorX : -1,
                       Render_None);
        } else {
            render(0, y, line.data, 0, line.data.size(), c ? mCursorX : -1, Render_None);
        }
    }
}

bool Terminal::isSelected(int y, int *start, int *length) const
{
#if 0
    if (!mHasSelection && (mSelectionStartY != mSelectionEndY || mSelectionStartX != mSelectionEndX)) {
        *start = *length = 0;
        return false;
    }
    const int minY = std::min(mSelectionStartY, mSelectionEndY);
    const int maxY = std::max(mSelectionStartY, mSelectionEndY);
    const int minX = std::min(mSelectionStartX, mSelectionEndX);
    const int maxX = std::max(mSelectionStartX, mSelectionEndX);

    if (y > minY && y < maxY) {
        *start = 0;
        *length = mScrollback[y].data.length();
    } else if (y == minY && y == maxY) {
        *start = minX;
        *length = mScrollback[y].data.length() - minX - maxX;
    } else if (y == minY) {
        *start = minX;
        *length = mScrollback[y].data.length() - minX;
    } else if (y == maxY) {
        *start = 0;
        *length = mScrollback[y].data.length() - maxX;
    } else {
        *start = *length = 0;
        return false;
    }
    return true;
#endif
    *start = *length = 0;
    return false;
}

void Terminal::keyPressEvent(const KeyEvent *event)
{
    if (event->key == Key_F12) {
        // for (int i=0; i<mScrollback.size(); ++i) {
        //     printf("%d/%d: \"%s\"\n", i, mScrollback.size(), mScrollback[i].data.c_str());
        // }

        return;
    }
    // DEBUG("Got keypress \"%s\" %d\n", toPrintable(event->text).c_str(), event->count);
    std::string text = event->text;
    if (text.empty()) {
        switch (event->key) {
        case Key_Left: text = "\02"; break;
        case Key_Right: text = "\06"; break;
        case Key_Up: text = "\020"; break;
        case Key_Down: text = "\016"; break;
        default:
            break;
        }
    }
    if (!text.empty() && event->count) {
        const char *ch = text.c_str();
        int bytes = text.size();
        for (size_t i=0; i<event->count; ++i) {
            int ret;
            DEBUG("Writing [%s] to master", toPrintable(text).c_str());
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

void Terminal::keyReleaseEvent(const KeyEvent *event)
{
    DEBUG("GOT KEYRELEASE [%s] %d %zu\n", event->text.c_str(), event->autoRepeat, event->count);
}

void Terminal::mousePressEvent(const MouseEvent *event)
{
    DEBUG("Got mouse press event button %s buttons %s %d,%d (%d,%d)",
          MouseEvent::buttonName(event->button).c_str(),
          MouseEvent::buttonName(event->buttons).c_str(),
          event->x, event->y, event->globalX, event->globalY);
}

void Terminal::mouseReleaseEvent(const MouseEvent *event)
{
    DEBUG("Got mouse release event button %s buttons %s %d,%d (%d,%d)",
          MouseEvent::buttonName(event->button).c_str(),
          MouseEvent::buttonName(event->buttons).c_str(),
          event->x, event->y, event->globalX, event->globalY);
}

void Terminal::mouseMoveEvent(const MouseEvent *event)
{
    DEBUG("Got mouse move event buttons %s %d,%d (%d,%d)",
          MouseEvent::buttonName(event->buttons).c_str(),
          event->x, event->y, event->globalX, event->globalY);
}

void Terminal::readFromFD()
{
    char buf[1024];
#ifndef NDEBUG
    memset(buf, 0, sizeof(buf));
#endif
    int ret;
    EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf) - 1));
    if (ret == -1) {
        ERROR("Failed to read from mMasterFD %d %s", errno, strerror(errno));
        mExitCode = 1;
        mPlatform->quit();
        return;
    } else if (ret == 0) {
        mPlatform->quit();
        return;
    }
    DEBUG("readFromFD: \"%s\"", toPrintable(buf, ret).c_str());
    const int len = ret;
    const int lineCount = mLines.size();
    assert(!mLines.empty());
    auto resetToNormal = [this]() {
        assert(mState == InEscape);
        mState = Normal;
        mEscapeIndex = 0;
#ifndef NDEBUG
        memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
#endif
    };
    Line *currentLine = &mLines.back();
    bool lineWasEmpty = currentLine->data.empty();
    for (int i=0; i<len; ++i) {
        switch (mState) {
        case Normal:
            switch (buf[i]) {
            case 0x1b: // escape
                mState = InEscape;
                assert(mEscapeIndex == 0);
                break;
            case '\n': // newline
                mScrollbackLength += currentLine->lineBreaks.size() + 1;
                mLines.push_back(Line());
                currentLine = &mLines.back();
                break;
            case '\r': // handle all these here?
                mCursorX = 0;
                break;
            case '\b':
                if (mCursorX > 0)
                    --mCursorX;
                break;
            case '\v':
            case '\t':
            case '\a':
                break;
            default:
                if (buf[i] & 0x80) {
                    assert(mUtf8Index == 0);
                    mUtf8Buffer[mUtf8Index++] = buf[i];
                    mState = InUtf8;
                } else {
                    currentLine->data += buf[i];
                    ++mCursorX;
                }
                break;
            }
            break;
        case InUtf8:
            assert(mUtf8Index > 0 && mUtf8Index < 6);
            mUtf8Buffer[mUtf8Index++] = buf[i];
            if (!(buf[i] & 0x80)) {
                const std::string str(mUtf8Buffer, mUtf8Index);
                currentLine->data += str;
                mCursorX += str.size();
#warning this should do error checking witgh QTextDecoder
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif

                mState = Normal;
            } else if (mUtf8Index == 6) {
                ERROR("Bad utf8"); // ### write what the data is?
                for (int j=0; j<mUtf8Index; ++j) {
                    currentLine->data.push_back(mUtf8Buffer[j]);
                }
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mState = Normal;
            }
            break;
        case InEscape:
            assert(mEscapeIndex < sizeof(mEscapeBuffer));
            mEscapeBuffer[mEscapeIndex++] = buf[i];
            DEBUG("Adding escape byte [%d] %s %d -> %s", mEscapeIndex - 1,
                  toPrintable(buf + i, 1).c_str(),
                  i, toPrintable(mEscapeBuffer, mEscapeIndex).c_str());
            switch (mEscapeBuffer[0]) {
            case SS2:
            case SS3:
            case DCS:
            case ST:
            case OSX:
            case SOS:
            case PM:
            case APC:
                ERROR("Unhandled escape sequence %s", escapeSequenceName(static_cast<EscapeSequence>(mEscapeBuffer[0])));
                resetToNormal();
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
            case RIS: {
                Line *line = &mLines.back();
                Command cmd;
                cmd.type = Command::ResetToInitialState;
                cmd.idx = line->data.size();
                line->commands.push_back(std::move(cmd));
                resetToNormal();
                break; }
            case VB: {
                event(VisibleBell);
                resetToNormal();
                break; }
            default:
                ERROR("Unknown escape sequence %s\n", toPrintable(mEscapeBuffer, 1).c_str());
                resetToNormal();
                break;
            }

            break;
        }
    }

    if (mLines.size() != lineCount || mCursorY == -1 || (lineWasEmpty && !currentLine->data.empty())) {
        mCursorX = mLines.back().data.size();
        mCursorY = mLines.size() - 1;
        DEBUG("Got cursor %d %d %lu", mCursorY, mCursorX, mLines.back().data.size());
    }
    // ### should only update when something requires an updated
    event(Update);
    event(ScrollbackChanged);

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
        if (end != mEscapeBuffer + mEscapeIndex - 1 || l > std::numeric_limits<int>::max()) {
            return -1;
        }
        return static_cast<int>(l);
    };

    Action action;
    switch (mEscapeBuffer[mEscapeIndex - 1]) {
    case CUU: // Cursor up
    case CUD: // Cursor down
    case CUF: // Cursor forward
    case CUB: // Cursor back
    case CNL: // Cursor next line
    case CPL: // Cursor previous line
    case CHA:  // Cursor horizontal absolute
    case DCH: { // DeleteChars
        const int count = readCount(1);
        if (count == -1) {
            ERROR("Invalid parameters for CSI %s", csiSequenceName(static_cast<CSISequence>(mEscapeBuffer[mEscapeIndex - 1])));
            break;
        }
        assert(count >= 0);
        action.count = count;
        switch (mEscapeBuffer[mEscapeIndex - 1]) {
        case CUU: action.type = Action::CursorUp; break;
        case CUD: action.type = Action::CursorDown; break;
        case CUF: action.type = Action::CursorForward; break;
        case CUB: action.type = Action::CursorBack; break;
        case CNL: action.type = Action::CursorNextLine; break;
        case CPL: action.type = Action::CursorPreviousLine; break;
        case CHA: action.type = Action::CursorHorizontalAbsolute; break;
        case DCH: action.type = Action::DeleteChars; break;
        }
        break; }
    case HVP: // Cursor position
    case CUP: { // Cursor position
        char *end;
        action.x = action.y = 1;
        action.type = Action::CursorPosition;
        const unsigned long x = strtoul(mEscapeBuffer + 1, &end, 10);
        if (end == mEscapeBuffer + mEscapeIndex - 1) { // no ; column not set, default to 1
            if (mEscapeIndex != 2) { // optional row
                if (!x) {
                    ERROR("Invalid CSI CUP error 1");
                    action.type = Action::Invalid;
                } else {
                    action.x = x;
                }
            }
        } else if (*end == ';') {
            if (mEscapeBuffer[1] != ';') { // default row 1, already set
                if (x == 0) {
                    ERROR("Invalid CSI CUP error 2");
                    action.type = Action::Invalid;
                    break;
                }
                action.x = x;
            }
            if (end + 1 < mEscapeBuffer + mEscapeIndex - 1) { // optional column
                const unsigned long y = strtoul(end + 1, &end, 10);
                if (end != mEscapeBuffer + mEscapeIndex - 1 || !y) { // bad column
                    ERROR("Invalid CSI CUP error 3");
                    action.type = Action::Invalid;
                    break;
                } else {
                    action.y = y;
                }
            }
        }
        break; }
    case ED:  // Erase in display
        switch (readCount(0)) {
        case 0:
            action.type = Action::ClearToEndOfScreen;
            break;
        case 1:
            action.type = Action::ClearToBeginningOfScreen;
            break;
        case 2:
            action.type = Action::ClearScreen;
            break;
        default:
            ERROR("Invalid CSI ED %d", readCount(0));
            break;
        }
        break;
    case EL: // Erase in line
        switch (readCount(0)) {
        case 0:
            action.type = Action::ClearToEndOfLine;
            break;
        case 1:
            action.type = Action::ClearToBeginningOfLine;
            break;
        case 2:
            action.type = Action::ClearLine;
            break;
        default:
            ERROR("Invalid CSI EL %d", readCount(0));
            break;
        }
        break;
    case SU: { // Scroll up
        const int n = readCount(1);
        if (n <= 0) {
            ERROR("Invalid CSI SU %d", n);
        } else {
            action.type = Action::ScrollUp;
            action.count = n;
        }
        break; }
    case SD: { // Scroll down
        const int n = readCount(1);
        if (n <= 0) {
            ERROR("Invalid CSI SD %d", n);
        } else {
            action.type = Action::ScrollDown;
            action.count = n;
        }
        break; }
    case SGR: // Select graphic rendition
        processSGR(&action);
        break;
    case AUX: // AUX port
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
    case DSR: // Device status report
        if (mEscapeIndex != 3 || mEscapeBuffer[1] != '6') {
            ERROR("Invalid AUX DSR command (%d)", mEscapeIndex);
            break;

        }
        break;
    case SCP: // Save cursor position
        if (mEscapeIndex != 2) {
            ERROR("Invalid AUX SCP command (%d)", mEscapeIndex);
        } else {
            action.type = Action::SaveCursorPosition;
        }
        break;
    case RCP: // Restore cursor position
        if (mEscapeIndex != 2) {
            ERROR("Invalid AUX RCP command (%d)", mEscapeIndex);
        } else {
            action.type = Action::RestoreCursorPosition;
        }
        break;
    default:
        ERROR("Unknown CSI final byte 0x%x", mEscapeBuffer[mEscapeIndex - 1]);
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
    machtime = machtime * info.numer / (info.denom * 1000); // microseconds
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

unsigned long long Terminal::mono()
{
    timeval time;
    if (gettime(&time)) {
        return (time.tv_sec * static_cast<uint64_t>(1000)) + (time.tv_usec / static_cast<uint64_t>(1000));
    }
    return 0;
}

void Terminal::onAction(const Action *action)
{
    assert(action);
    assert(action->type != Action::Invalid);
    DEBUG("Got action %s %d %d %d", Action::typeName(action->type), action->count, action->x, action->y);
    switch (action->type) {
    case Action::ClearLine:
        mLines.back().data.clear(); // modify cursor pos?
        break;
    case Action::ClearToBeginningOfLine: {
        Line &line = mLines.back();
        if (mCursorX > 0) {
            line.data.erase(0, mCursorX);
        }
        break; }
    case Action::ClearToEndOfLine: {
        Line &line = mLines.back();
        if (mCursorX < line.data.size()) {
            line.data.erase(mCursorX, line.data.size() - mCursorX);
        }
        break; }
    case Action::CursorForward:
        mCursorX += action->count;
        break;
    case Action::CursorBack:
        mCursorX = action->count > mCursorX ? 0 : mCursorX - action->count;
        break;
    case Action::DeleteChars:
        mLines.back().data.erase(mCursorX, action->count);
        break;
    default:
        break;
    }
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
    case ScrollUp: return "ScrollUp";
    case ScrollDown: return "ScrollDown";
    case SelectGraphicRendition: return "SelectGraphicRendition";
    case AUXPortOn: return "AUXPortOn";
    case AUXPortOff: return "AUXPortOff";
    case DeviceStatusReport: return "DeviceStatusReport";
    case SaveCursorPosition: return "SaveCursorPosition";
    case RestoreCursorPosition: return "RestoreCursorPosition";
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
    case RCP: return "RCP";
    case DCH: return "DCH";
    }
    return nullptr;
}

void Terminal::processSGR(Action *action)
{
    assert(mEscapeBuffer[0] == CSI);
    assert(mEscapeBuffer[mEscapeIndex - 1] == 'm');
}

const char *Terminal::eventName(Event event)
{
    switch (event) {
    case Update: return "Update";
    case ScrollbackChanged: return "ScrollbackChanged";
    case VisibleBell: return "VisibleBell";
    }
    abort();
    return nullptr;
}
