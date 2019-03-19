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
        const std::string &line = mScrollback[y];
        if (isSelected(y, &start, &length)) {
            assert(start + length <= line.size());
            if (start > 0)
                render(0, y - mY, line.c_str(), start, Render_None);
            render(start, y - mY, line.c_str() + start, length, Render_Selected);
            if (start + length < line.size())
                render(start + length, y - mY, line.c_str() + start + length, line.size() - start - length, Render_None);
        } else {
            render(0, y - mY, line.c_str(), line.size(), Render_None);
        }
    }
}

void Terminal::addText(const char *str, size_t len)
{
    if (mScrollback.empty())
        mScrollback.push_back(std::string());
    size_t last = 0;
    for (size_t idx = 0; idx<len; ++idx) {
        if (str[idx] == '\n') {
            assert(!mScrollback.empty());
            mScrollback[mScrollback.size() - 1].append(str + last, idx - last);
            mScrollback.push_back(std::string());
            last = idx + 1;
        }
    }

    if (last < len) {
        assert(!mScrollback.empty());
        mScrollback[mScrollback.size() - 1].append(str + last, len - last);
    }
    event(Update);
    event(ScrollbackChanged);
}

bool Terminal::isSelected(size_t y, size_t *start, size_t *length) const
{
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
        *length = mScrollback[y].length();
    } else if (y == minY && y == maxY) {
        *start = minX;
        *length = mScrollback[y].length() - minX - maxX;
    } else if (y == minY) {
        *start = minX;
        *length = mScrollback[y].length() - minX;
    } else if (y == maxY) {
        *start = 0;
        *length = mScrollback[y].length() - maxX;
    } else {
        *start = *length = 0;
        return false;
    }
    return true;
}

void Terminal::keyPressEvent(const KeyEvent &event)
{
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
    addText(buf, ret);
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
