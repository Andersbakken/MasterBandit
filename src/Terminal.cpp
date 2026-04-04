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

Terminal::Terminal(Platform *platform, TerminalCallbacks callbacks)
    : TerminalEmulator(std::move(callbacks))
    , mPlatform(platform)
{
}

Terminal::~Terminal()
{
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
        setenv("COLORTERM", "truecolor", 1);

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

void Terminal::resize(int width, int height)
{
    TerminalEmulator::resize(width, height);
    if (mMasterFD != -1) {
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(width);
        ws.ws_row = static_cast<unsigned short>(height);
        ioctl(mMasterFD, TIOCSWINSZ, &ws);
    }
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
        if (errno != EIO) {
            ERROR("Failed to read from mMasterFD %d %s", errno, strerror(errno));
        }
        mPlatform->terminalExited(this);
        return;
    } else if (ret == 0) {
        mPlatform->terminalExited(this);
        return;
    }

    injectData(buf, static_cast<size_t>(ret));
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
