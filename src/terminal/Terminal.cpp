#include "Terminal.h"
#include "Utils.h"
#include "Log.h"
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

Terminal::Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks)
    : TerminalEmulator(std::move(callbacks))
    , mPlatformCbs(std::move(platformCbs))
{
}

Terminal::~Terminal()
{
    if (mWritePollActive) {
        uv_poll_stop(&mWritePoll);
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

    // Non-blocking so readFromFD can drain the buffer in a loop
    fcntl(mMasterFD, F_SETFL, fcntl(mMasterFD, F_GETFL) | O_NONBLOCK);

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

        if (!mOptions.cwd.empty()) {
            chdir(mOptions.cwd.c_str());
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
        FATAL("Failed to fork -> %d %s", errno, strerror(errno));
        return false;
    default:
        EINTRWRAP(ret, ::close(slaveFD));
        break;
    }
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
    if (!mResizePending || mMasterFD == -1) return;
    mResizePending = false;
    struct winsize ws = {};
    ws.ws_col = static_cast<unsigned short>(this->width());
    ws.ws_row = static_cast<unsigned short>(this->height());
    ioctl(mMasterFD, TIOCSWINSZ, &ws);
}

void Terminal::readFromFD()
{
    // Drain all available data to avoid rendering intermediate states.
    char buf[65536];
    for (;;) {
        int ret;
        EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf)));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno != EIO) {
                ERROR("Failed to read from mMasterFD %d %s", errno, strerror(errno));
            }
            if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
            return;
        } else if (ret == 0) {
            if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
            return;
        }
        injectData(buf, static_cast<size_t>(ret));
    }
}

void Terminal::writeToPTY(const char* data, size_t len)
{
    // Try to write directly first
    while (len > 0 && mWriteQueue.empty()) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, data, len));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            FATAL("Failed to write to master %d %s", errno, strerror(errno));
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
            uv_poll_init(mLoop, &mWritePoll, mMasterFD);
            mWritePoll.data = this;
            uv_poll_start(&mWritePoll, UV_WRITABLE, onWritePollReady);
            mWritePollActive = true;
        }
    }
}

void Terminal::onWritePollReady(uv_poll_t* handle, int status, int events)
{
    if (status < 0) return;
    auto* self = static_cast<Terminal*>(handle->data);
    if (events & UV_WRITABLE) self->flushWriteQueue();
}

void Terminal::flushWriteQueue()
{
    while (!mWriteQueue.empty()) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, mWriteQueue.data(), mWriteQueue.size()));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; // try again next poll
            FATAL("Failed to write to master %d %s", errno, strerror(errno));
            if (mPlatformCbs.quit) mPlatformCbs.quit();
            return;
        }
        mWriteQueue.erase(mWriteQueue.begin(), mWriteQueue.begin() + ret);
    }
    // Queue drained — stop write poll
    if (mWritePollActive) {
        uv_poll_stop(&mWritePoll);
        uv_close(reinterpret_cast<uv_handle_t*>(&mWritePoll), nullptr);
        mWritePollActive = false;
    }
}

void Terminal::writeToOutput(const char* data, size_t len)
{
    writeToPTY(data, len);
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
