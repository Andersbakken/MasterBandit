#include "Terminal.h"
#include "Utils.h"
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
#include <signal.h>
#include <algorithm>
#ifdef __APPLE__
#include <libproc.h>
#endif

Terminal::Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks)
    : TerminalEmulator(std::move(callbacks))
    , mPlatformCbs(std::move(platformCbs))
{
}

Terminal::~Terminal()
{
    if (mWritePollActive && mLoop && mMasterFD >= 0) {
        mLoop->updateFd(mMasterFD, EventLoop::FdEvents::Readable);
        mWritePollActive = false;
    }
    if (mMasterFD != -1) ::close(mMasterFD);
}

bool Terminal::init(const TerminalOptions &options)
{
    mOptions = options;
    // Re-initialize document with configured scrollback capacity
    resetScrollback(mOptions.resolvedScrollback());

    mMasterFD = posix_openpt(O_RDWR | O_NOCTTY);
    if (mMasterFD == -1) {
        spdlog::critical("Failed to posix_openpt -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    }

    if (grantpt(mMasterFD) == -1) {
        spdlog::critical("Failed to grantpt -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    }

    if (unlockpt(mMasterFD) == -1) {
        spdlog::critical("Failed to unlockpt -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    }

    // Non-blocking so readFromFD can drain the buffer in a loop
    fcntl(mMasterFD, F_SETFL, fcntl(mMasterFD, F_GETFL) | O_NONBLOCK);

    char *slaveName = ptsname(mMasterFD);
    if (!slaveName) {
        spdlog::critical("Failed to ptsname -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    }

    int slaveFD;
    EINTRWRAP(slaveFD, open(slaveName, O_RDWR | O_NOCTTY));
    if (slaveFD == -1) {
        spdlog::critical("Failed to open slave fd -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    }

    const pid_t pid = fork();
    int ret;
    switch (pid) {
    case 0:
        EINTRWRAP(ret, ::close(mMasterFD));
        setsid();
        if (ioctl(slaveFD, TIOCSCTTY, NULL) == -1) {
            spdlog::critical("Failed to ioctl slave fd in slave -> {} {}", errno, strerror(errno)); std::abort();
            return false;
        }

        for (int i=0; i<3; ++i) {
            EINTRWRAP(ret, dup2(slaveFD, i));
            if (ret == -1) {
                spdlog::critical("Failed to dup2({}) slave fd in slave -> {} {}", i, errno, strerror(errno)); std::abort();
                return false;
            }
        }

        EINTRWRAP(ret, ::close(slaveFD));

        if (!mOptions.cwd.empty()) {
            if (chdir(mOptions.cwd.c_str()) == -1) {
                spdlog::warn("Failed to chdir to '{}' -> {} {}", mOptions.cwd, errno, strerror(errno));
            }
        }

        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        setenv("LOGNAME", mOptions.user.c_str(), 1);
        setenv("USER", mOptions.user.c_str(), 1);
        setenv("SHELL", mOptions.shell.c_str(), 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "MasterBandit", 1);
        unsetenv("LC_TERMINAL");
        unsetenv("KONSOLE_VERSION");

        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);

        if (!mOptions.command.empty()) {
            // Run a specific command (e.g. pager) instead of a shell
            execl("/bin/sh", "sh", "-c", mOptions.command.c_str(), nullptr);
        } else {
            execl(mOptions.shell.c_str(), mOptions.shell.c_str(), nullptr);
        }
        return false;

        break;
    case -1:
        spdlog::critical("Failed to fork -> {} {}", errno, strerror(errno)); std::abort();
        return false;
    default:
        EINTRWRAP(ret, ::close(slaveFD));
        break;
    }
    return true;
}

bool Terminal::initHeadless(const TerminalOptions& options)
{
    mOptions = options;
    mHeadless = true;
    resetScrollback(mOptions.resolvedScrollback());
    // No PTY, no fork. mMasterFD stays -1.
    return true;
}

void Terminal::resize(int width, int height)
{
    int oldW = this->width(), oldH = this->height();
    TerminalEmulator::resize(width, height);
    if (this->width() != oldW || this->height() != oldH) {
        mResizePending = true;
    }
}

void Terminal::flushPendingResize()
{
    if (!mResizePending || mMasterFD == -1 || mHeadless) return;
    mResizePending = false;
    struct winsize ws = {};
    ws.ws_col = static_cast<unsigned short>(this->width());
    ws.ws_row = static_cast<unsigned short>(this->height());
    auto& cbs = callbacks();
    if (cbs.cellPixelWidth && cbs.cellPixelHeight) {
        ws.ws_xpixel = static_cast<unsigned short>(cbs.cellPixelWidth() * ws.ws_col);
        ws.ws_ypixel = static_cast<unsigned short>(cbs.cellPixelHeight() * ws.ws_row);
    }
    ioctl(mMasterFD, TIOCSWINSZ, &ws);
}

void Terminal::readFromFD()
{
    // Accumulate data into coalesce buffer without injecting.
    // On macOS, PTY buffers are small (4-16KB) so each kqueue event
    // only delivers ~1KB. We buffer across events and inject all at
    // once via flushReadBuffer() before rendering.
    char buf[65536];
    for (;;) {
        int ret;
        EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf)));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno != EIO)
                spdlog::error("Failed to read from mMasterFD {} {}", errno, strerror(errno));
            if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
            return;
        } else if (ret == 0) {
            if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
            return;
        }
        mReadCoalesceBuffer.insert(mReadCoalesceBuffer.end(), buf, buf + ret);
    }
}

void Terminal::flushReadBuffer()
{
    if (mReadCoalesceBuffer.empty()) return;

    if (mPlatformCbs.shouldFilterOutput && mPlatformCbs.shouldFilterOutput()) {
        std::string s(mReadCoalesceBuffer.begin(), mReadCoalesceBuffer.end());
        mPlatformCbs.filterOutput(s);
        if (!s.empty())
            injectData(s.data(), s.size());
    } else {
        injectData(mReadCoalesceBuffer.data(), mReadCoalesceBuffer.size());
    }
    mReadCoalesceBuffer.clear();

    // Check for foreground process change
    if (callbacks().onForegroundProcessChanged) {
        pid_t pgid = tcgetpgrp(mMasterFD);
        if (pgid > 0 && pgid != mLastFgPgid) {
            mLastFgPgid = pgid;
            callbacks().onForegroundProcessChanged(foregroundProcess());
        }
    }
}

void Terminal::writeToPTY(const char* data, size_t len)
{
    // Try to write directly first
    while (len > 0 && mWriteQueue.empty()) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, data, len));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            spdlog::critical("Failed to write to master {} {}", errno, strerror(errno)); std::abort();
            if (mPlatformCbs.quit) mPlatformCbs.quit();
            return;
        }
        data += ret;
        len -= ret;
    }

    // Queue remaining data and start write poll
    if (len > 0) {
        mWriteQueue.insert(mWriteQueue.end(), data, data + len);
        if (!mWritePollActive && mLoop) {
            mLoop->updateFd(mMasterFD,
                EventLoop::FdEvents::Readable | EventLoop::FdEvents::Writable);
            mWritePollActive = true;
        }
    }
}

void Terminal::flushWriteQueue()
{
    while (!mWriteQueue.empty()) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, mWriteQueue.data(), mWriteQueue.size()));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; // try again next poll
            spdlog::critical("Failed to write to master {} {}", errno, strerror(errno)); std::abort();
            if (mPlatformCbs.quit) mPlatformCbs.quit();
            return;
        }
        mWriteQueue.erase(mWriteQueue.begin(), mWriteQueue.begin() + ret);
    }
    // Queue drained — drop writable watch
    if (mWritePollActive && mLoop) {
        mLoop->updateFd(mMasterFD, EventLoop::FdEvents::Readable);
        mWritePollActive = false;
    }
}

void Terminal::writeToOutput(const char* data, size_t len)
{
    if (mHeadless) {
        // No PTY — route to script via callback
        if (mPlatformCbs.onInput)
            mPlatformCbs.onInput(data, len);
        return;
    }
    if (mPlatformCbs.shouldFilterInput && mPlatformCbs.shouldFilterInput()) {
        std::string s(data, len);
        mPlatformCbs.filterInput(s);
        if (!s.empty())
            writeToPTY(s.data(), s.size());
    } else {
        writeToPTY(data, len);
    }
}

void Terminal::pasteText(const std::string& text)
{
    if (bracketedPaste()) {
        writeToPTY("\x1b[200~", 6);
    }
    writeToPTY(text.c_str(), text.size());
    if (bracketedPaste()) {
        writeToPTY("\x1b[201~", 6);
    }
}

std::string Terminal::foregroundProcess() const
{
    if (mMasterFD < 0) return {};

    pid_t pgid = tcgetpgrp(mMasterFD);
    if (pgid < 0) return {};

#ifdef __APPLE__
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pgid, pathbuf, sizeof(pathbuf)) > 0) {
        // Extract basename
        const char* slash = strrchr(pathbuf, '/');
        return slash ? slash + 1 : pathbuf;
    }
    return {};
#else
    // Linux: read /proc/<pid>/comm
    char comm[256];
    snprintf(comm, sizeof(comm), "/proc/%d/comm", static_cast<int>(pgid));
    FILE* f = fopen(comm, "r");
    if (!f) return {};
    char name[256] = {};
    if (fgets(name, sizeof(name), f)) {
        // Strip trailing newline
        size_t len = strlen(name);
        if (len > 0 && name[len - 1] == '\n') name[len - 1] = '\0';
    }
    fclose(f);
    return name;
#endif
}
