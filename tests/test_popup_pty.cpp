#include "Terminal.h"
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Spec tests for pty-backed popups: a popup child Terminal created with
// pty=true owns a forkless PTY pair. External apps open ttyName() and write
// bytes that render in the popup; the popup's keyboard output is readable
// from the same device. MB holds its own slave fd open, so external writers
// coming and going never EOF the master.

namespace {

struct PtyPopupFixture
{
    std::unique_ptr<Terminal> term;
    bool popupExited = false;

    PtyPopupFixture()
    {
        PlatformCallbacks pcbs;
        TerminalCallbacks cbs;
        term = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
        TerminalOptions opts;
        opts.scrollbackLines = 0;
        term->initHeadless(opts);
        term->resize(80, 24);
    }

    Terminal *addPtyPopup(const std::string &id, int x, int y, int w, int h)
    {
        PlatformCallbacks pcbs;
        pcbs.onTerminalExited = [this](Terminal *)
        {
            popupExited = true;
        };
        return term->createPopup(id, x, y, w, h, std::move(pcbs), true);
    }
};

// Synchronous parse path: drain the master fd, then parse on this thread.
void drain(Terminal *popup)
{
    popup->readFromFD();
    popup->flushReadBuffer();
}

std::string rowText(Terminal *popup, int row)
{
    std::string result;
    for (int col = 0; col < popup->width(); ++col) {
        char32_t cp = popup->grid().cell(col, row).wc;
        result += (cp == 0 || cp > 0x7f) ? ' ' : static_cast<char>(cp);
    }
    auto end = result.find_last_not_of(' ');
    return end == std::string::npos ? "" : result.substr(0, end + 1);
}

int openSlave(Terminal *popup)
{
    return ::open(popup->ttyName().c_str(), O_RDWR | O_NOCTTY);
}

} // namespace

TEST_CASE("pty popup: create exposes a working tty")
{
    PtyPopupFixture f;
    Terminal *popup = f.addPtyPopup("p", 0, 0, 20, 5);
    REQUIRE(popup != nullptr);
    CHECK(popup->masterFD() >= 0);
    CHECK(!popup->ttyName().empty());
    CHECK(!popup->isHeadless());

    int fd = openSlave(popup);
    REQUIRE(fd >= 0);
    CHECK(::write(fd, "hello", 5) == 5);
    drain(popup);
    CHECK(rowText(popup, 0) == "hello");
    ::close(fd);
}

TEST_CASE("pty popup: headless popup has no tty")
{
    PtyPopupFixture f;
    PlatformCallbacks pcbs;
    Terminal *popup = f.term->createPopup("h", 0, 0, 20, 5, std::move(pcbs));
    REQUIRE(popup != nullptr);
    CHECK(popup->masterFD() == -1);
    CHECK(popup->ttyName().empty());
}

TEST_CASE("pty popup: keyboard output is readable from the slave")
{
    PtyPopupFixture f;
    Terminal *popup = f.addPtyPopup("p", 0, 0, 20, 5);
    REQUIRE(popup != nullptr);

    int fd = openSlave(popup);
    REQUIRE(fd >= 0);
    // Default termios is canonical+echo; raw-mode the device so the read
    // below doesn't wait for a newline and nothing echoes back into the
    // popup's render stream.
    struct termios tio;
    REQUIRE(tcgetattr(fd, &tio) == 0);
    cfmakeraw(&tio);
    REQUIRE(tcsetattr(fd, TCSANOW, &tio) == 0);

    // Non-headless writeToOutput routes to the PTY master.
    popup->writeText("hi");
    char buf[16];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    REQUIRE(n == 2);
    CHECK(std::string(buf, n) == "hi");
    ::close(fd);
}

TEST_CASE("pty popup: survives writers opening and closing")
{
    PtyPopupFixture f;
    Terminal *popup = f.addPtyPopup("p", 0, 0, 20, 5);
    REQUIRE(popup != nullptr);

    int fd = openSlave(popup);
    REQUIRE(fd >= 0);
    CHECK(::write(fd, "a", 1) == 1);
    ::close(fd);
    // Last external writer is gone; the held slave fd keeps the device
    // alive, so this drains "a" and then hits EAGAIN — not EOF/EIO.
    drain(popup);
    CHECK_FALSE(f.popupExited);

    // A second writer generation works.
    fd = openSlave(popup);
    REQUIRE(fd >= 0);
    CHECK(::write(fd, "b", 1) == 1);
    drain(popup);
    CHECK(rowText(popup, 0) == "ab");
    CHECK_FALSE(f.popupExited);
    ::close(fd);
}

TEST_CASE("pty popup: duplicate id fails")
{
    PtyPopupFixture f;
    REQUIRE(f.addPtyPopup("p", 0, 0, 20, 5) != nullptr);
    CHECK(f.addPtyPopup("p", 2, 2, 10, 3) == nullptr);
}

TEST_CASE("pty popup: TIOCGWINSZ reports the popup size")
{
    PtyPopupFixture f;
    Terminal *popup = f.addPtyPopup("p", 0, 0, 20, 5);
    REQUIRE(popup != nullptr);
    // createPopup resizes after initPtyOnly; the pending TIOCSWINSZ is
    // delivered by flushPendingResize (per-tick sweep in production).
    popup->flushPendingResize();

    int fd = openSlave(popup);
    REQUIRE(fd >= 0);
    struct winsize ws = {};
    REQUIRE(ioctl(fd, TIOCGWINSZ, &ws) == 0);
    CHECK(ws.ws_col == 20);
    CHECK(ws.ws_row == 5);
    ::close(fd);

    // Resize propagates on the next flush.
    REQUIRE(f.term->resizePopup("p", 1, 1, 30, 8));
    popup->flushPendingResize();
    fd = openSlave(popup);
    REQUIRE(fd >= 0);
    REQUIRE(ioctl(fd, TIOCGWINSZ, &ws) == 0);
    CHECK(ws.ws_col == 30);
    CHECK(ws.ws_row == 8);
    ::close(fd);
}

TEST_CASE("pty popup: enqueueParseBytes is ordered with pty bytes")
{
    PtyPopupFixture f;
    Terminal *popup = f.addPtyPopup("p", 0, 0, 20, 5);
    REQUIRE(popup != nullptr);

    int fd = openSlave(popup);
    REQUIRE(fd >= 0);
    CHECK(::write(fd, "ex", 2) == 2);
    popup->readFromFD();
    // No parse submit fn wired → synchronous drain, after the pty bytes.
    popup->enqueueParseBytes("tra", 3);
    CHECK(rowText(popup, 0) == "extra");
    ::close(fd);
}
