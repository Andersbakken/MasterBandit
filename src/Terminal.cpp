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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

Terminal::Terminal()
{
}

bool Terminal::init(const Options &options)
{
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
        execle(mOptions.shell.c_str(), mOptions.shell.c_str(), NULL, environ);
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

void Terminal::scroll(size_t x, size_t y)
{
    mX = x;
    mY = y;
    event(Update);
}

void Terminal::resize(size_t width, size_t height)
{
    mWidth = width;
    mHeight = height;
    event(Update);
}

void Terminal::render()
{
    const size_t max = std::min(mY + mHeight, mScrollback.size());
    // printf("RENDER %zu mHeight %zu mY %zu max %zu\n", mScrollback.size(), mHeight, mY, max);
    for (size_t y = mY; y<max; ++y) {
        // printf("RENDERING %zu %s\n", y, mScrollback.at(y).c_str());
        size_t start, length;
        // const Line &line = mScrollback[y];
        // if (isSelected(y, &start, &length)) {
        //     assert(start + length <= line.data.size());
        //     if (start > 0)
        //         render(0, y - mY, line.data.c_str(), start, Render_None);
        //     render(start, y - mY, line.data.c_str() + start, length, Render_Selected);
        //     if (start + length < line.data.size())
        //         render(start + length, y - mY, line.data.c_str() + start + length, line.data.size() - start - length, Render_None);
        // } else {
        //     render(0, y - mY, line.data.c_str(), line.data.size(), Render_None);
        // }
    }
}

#if 0
void Terminal::addText(const char *str, size_t len)
{
    if (mScrollback.empty())
        mScrollback.push_back(Line());
    size_t last = 0;
    printf("Got chars: [");
    bool escape = false;
    Line *cur = &mScrollback[mScrollback.size() - 1];
    for (size_t idx = 0; idx<len; ++idx) {
        if (escape) {
            printf("GOT ESCAPE CODE %02x (%c)\n", static_cast<unsigned char>(str[idx]), str[idx]);
            escape = false;
        }
        switch (str[idx]) {
        case '\n':
            printf("\\n]\n");
            assert(!mScrollback.empty());
            cur->data.append(str + last, idx - last);
            mScrollback.push_back(Line());
            cur = &mScrollback[mScrollback.size() - 1];
            last = idx + 1;
            break;
        case '\b':
            cur->data.append(str + last, idx - last);
            last = idx + 1;
            printf("[Backspace]");
            if (cur->data.size())
                cur->data.resize(cur->data.size() - 1);
        // case '\r':
        //     printf("\\r]\n");
        //     cur->clear();
        //     last = idx + 1;
        //     break;
        case 0x1b: // ESC
            escape = true;
            break;
        default:
            if (std::isprint(str[idx])) {
                printf("%c", str[idx]);
            } else {
                printf("Unhandled unprintable: 0x%02x\n", str[idx]);
            }
        }
    }

    if (last < len) {
        printf("]\n");
        assert(!mScrollback.empty());
        cur->data.append(str + last, len - last);
    }
    event(Update);
    event(ScrollbackChanged);
}
#endif

bool Terminal::isSelected(size_t y, size_t *start, size_t *length) const
{
#if 0
    if (!mHasSelection && (mSelectionStartY != mSelectionEndY || mSelectionStartX != mSelectionEndX)) {
        *start = *length = 0;
        return false;
    }
    const size_t minY = std::min(mSelectionStartY, mSelectionEndY);
    const size_t maxY = std::max(mSelectionStartY, mSelectionEndY);
    const size_t minX = std::min(mSelectionStartX, mSelectionEndX);
    const size_t maxX = std::max(mSelectionStartX, mSelectionEndX);

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
}

void Terminal::keyPressEvent(const KeyEvent &event)
{
    if (event.key == KeyEvent::Key_F12) {
        // for (size_t i=0; i<mScrollback.size(); ++i) {
        //     printf("%zu/%zu: \"%s\"\n", i, mScrollback.size(), mScrollback[i].data.c_str());
        // }

        return;
    }
    // printf("GOT KEYPRESS [%s] %zu\n", event.text.c_str(), event.count);
    if (!event.text.empty()) {
        for (size_t i=0; i<event.count; ++i) {
            int ret;
            DEBUG("Writing [%s] to master", event.text.c_str());
            EINTRWRAP(ret, ::write(mMasterFD, event.text.c_str(), event.text.size()));
        }
    }
}

void Terminal::keyReleaseEvent(const KeyEvent &event)
{
    // printf("GOT KEYRELEASE [%s] %d %zu\n", event.text.c_str(), event.autoRepeat, event.count);
}

void Terminal::mousePressEvent(const MouseEvent &event)
{
}

void Terminal::mouseReleaseEvent(const MouseEvent &event)
{
}

void Terminal::mouseMoveEvent(const MouseEvent &event)
{
}

void Terminal::readFromFD()
{
    char buf[1024];
    int ret;
    EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf) - 1));
    if (ret == -1) {
        ERROR("Failed to read from mMasterFD %d %s", errno, strerror(errno));
        mExitCode = 1;
        quit();
        return;
    } else if (ret == 0) {
        quit();
        return;
    }
    assert((mState == Normal) == mRawBuffer.empty());

    auto processUtf8 = [this](const char *chars, size_t len) {
        try {
            utf8::utf8to16(chars, chars + len, std::back_inserter(mScrollback));
        } catch (const std::exception &e) {
            ERROR("Got exception processing utf8: %s", e.what());
            return false;
        }
        return true;

    };
    const size_t len = ret;
    size_t idx = 0;
    // while (mState != Normal && idx < len) {
    //     mRawBuffer.push_back(buf[idx++]);
    //     size_t consumed;
    //     if (mState == InUtf8) {
    //         if (processUtf8(
    //     } else {
    //         assert(mState == InEscape);
    //     }
    // }

    for (size_t i=0; i<len; ++i) {
        switch (mState) {
        case Normal:
            if (buf[i] == 0x1b) { // escape
                mState = InEscape;
                break;
            } else if (buf[i] & 0x80) {
                assert(mUtf8Index == 0);
                mUtf8Buffer[mUtf8Index++] = buf[i];
                mState = InUtf8;
            }
            break;
        case InUtf8:
            assert(mUtf8Index > 0 && mUtf8Index < 6);
            mUtf8Buffer[mUtf8Index++] = buf[i];
            if (!(buf[i] & 0x80)) {
                try {
                    utf8::utf8to16(mUtf8Buffer, mUtf8Buffer + mUtf8Index, std::back_inserter(mScrollback));
                } catch (const std::exception &e) {
                    ERROR("Got exception processing utf8: %s", e.what());
                    for (size_t j=0; j<mUtf8Index; ++j) {
                        mScrollback.push_back(mUtf8Buffer[j]);
                    }
                }
                mUtf8Index = 0;
                mState = Normal;
            } else if (mUtf8Index == 6) {
                ERROR("Bad utf8"); // ### write what the data is?
                for (size_t j=0; j<mUtf8Index; ++j) {
                    mScrollback.push_back(mUtf8Buffer[j]);
                }
                mUtf8Index = 0;
                mState = Normal;
            }
            break;
        case InEscape:
            assert(mEscapeIndex < sizeof(mEscapeBuffer));
            mEscapeBuffer[mEscapeIndex++] = buf[i];
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
                mState = Normal;
                mEscapeIndex = 0;
                break;
            case CSI:
                if (buf[i] < 0x20 || buf[i] >= 0x3f)
                {
                    ERROR("Invalid CSI sequence 0x%0x character", buf[i]);
                    mState = Normal;
                    mEscapeIndex = 0;
                } else if (buf[i] >= 0x40 && buf[i] <= 0x7e) {
                    processCSI();
                } else if (mEscapeIndex == sizeof(mEscapeBuffer)) {
                    ERROR("CSI sequence is too long %zu", sizeof(mEscapeBuffer));
                    mState = Normal;
                    mEscapeIndex = 0;
                }
                break;
            case RIS: {
                Command cmd;
                cmd.type = Command::ResetToInitialState;
                cmd.idx = mScrollback.size();
                mCommands.push_back(std::move(cmd));
                break; }
            }

            break;
        }
    }
    // mChunks.emplace_back(buf, ret, false);
    // processChunks();
    // if (!mChunks.empty())
    //     mChunks[mChunks.size() - 1].detach();
}

void Terminal::processCSI()
{
    assert(mState == InEscape);
    assert(mEscapeIndex >= 2);

    auto readCount = [this](int def = 1) {
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
    case CHA: { // Cursor horizontal absolute
        const int count = readCount();
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
        default: abort(); break;
        }
        break; }
    case HVP: // Cursor position
    case CUP: { // Cursor position
        char *end;
        action.row = action.column = 1;
        action.type = Action::CursorPosition;
        unsigned long row = strtoul(mEscapeBuffer + 1, &end, 10);
        if (end == mEscapeBuffer + mEscapeIndex - 1) { // no ; column not set, default to 1
            if (mEscapeIndex != 2) { // optional row
                if (!row) {
                    ERROR("Invalid CSI CUP error 1");
                    action.type = Action::Invalid;
                } else {
                    action.row = row;
                }
            }
        } else if (*end == ';') {
            if (mEscapeBuffer[1] != ';') { // default row 1, already set
                if (row == 0) {
                    ERROR("Invalid CSI CUP error 2");
                    action.type = Action::Invalid;
                    break;
                }
                action.row = row;
            }
            if (end + 1 < mEscapeBuffer + mEscapeIndex - 1) { // optional column
                unsigned long column = strtoul(end + 1, &end, 10);
                if (end != mEscapeBuffer + mEscapeIndex - 1 || !column) { // bad column
                    ERROR("Invalid CSI CUP error 3");
                    action.type = Action::Invalid;
                    break;
                } else {
                    action.column = column;
                }
            }
        }
        break; }
    case ED: // Erase in display
        break;
    case EL: // Erase in line
        break;
    case SU: // Scroll up
        break;
    case SD: // Scroll down
        break;
    case SGR: // Select graphic rendition
        break;
    case AUX: // AUX port
        if (mEscapeIndex != 3) {
            ERROR("Invalid AUX CSI command (%zu)", mEscapeIndex);
        } else if (mEscapeBuffer[1] == '4') {
            action.type = Action::AUXPortOff;
        } else if (mEscapeBuffer[1] == '5') {
            action.type = Action::AUXPortOn;
        } else {
            ERROR("Invalid AUX CSI command (0x%0x)", mEscapeBuffer[1]);
        }
        break;
    case DSR: // Device status report
        break;
    case SCP: // Save cursor position
        if (mEscapeIndex != 2) {
            ERROR("Invalid AUX SCP command (%zu)", mEscapeIndex);
        } else {
            action.type = Action::SaveCursorPosition;
        }
        break;
    case RCP: // Restore cursor position
        if (mEscapeIndex != 2) {
            ERROR("Invalid AUX RCP command (%zu)", mEscapeIndex);
        } else {
            action.type = Action::RestoreCursorPosition;
        }
        break;
    default:
        ERROR("Unknown CSI final byte 0x%x", mEscapeBuffer[mEscapeIndex - 1]);
        break;
    }

    mEscapeIndex = 0;
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
