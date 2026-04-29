#include "Terminal.h"
#include "Observability.h"
#include "Utils.h"
#include <spdlog/spdlog.h>

#include <assert.h>
#include <pwd.h>
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
#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <libproc.h>
#endif

Terminal::Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks)
    : TerminalEmulator(std::move(callbacks))
    , mPlatformCbs(std::move(platformCbs))
{
    // Embedded anchor evicted past the archive cap. Fires from inside
    // Document::evictToArchive while the parse worker holds mMutex,
    // so the map mutation here is already under that lock. We move
    // the unique_ptr onto mEvictedEmbeddeds (also under mMutex) and
    // set mEvictedHasItems so the main-thread tick can detect items
    // wait-free; main drains via drainEvictedEmbeddeds() once parsing
    // yields the lock between chunks. The render-thread snapshot path
    // also takes mMutex, so no cycle.
    document().setOnLineIdEvicted([this](uint64_t lineId) {
        auto it = mEmbedded.find(lineId);
        if (it == mEmbedded.end()) return;
        auto evicted = std::move(it->second);
        mEmbedded.erase(it);
        if (mFocusedEmbeddedLineId.load(std::memory_order_acquire) == lineId)
            mFocusedEmbeddedLineId.store(0, std::memory_order_release);
        mEvictedEmbeddeds.push_back({lineId, std::move(evicted)});
        mEvictedHasItems.store(true, std::memory_order_release);
        if (onPopupEvent) onPopupEvent();
    });
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

    // Set the initial PTY size before fork so the child process starts with
    // the correct dimensions. Without this, the kernel assigns a default
    // (often 0x0) and the child may read the wrong size before SIGWINCH
    // from flushPendingResize() arrives.
    if (this->width() > 0 && this->height() > 0) {
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(this->width());
        ws.ws_row = static_cast<unsigned short>(this->height());
        ioctl(slaveFD, TIOCSWINSZ, &ws);
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

        // Pick a sane starting directory. When launched via Launch Services
        // (open mb.app), the inherited cwd is "/", which is never what a user
        // wants for a new shell. Fall back to $HOME (or the passwd entry) if
        // no explicit cwd was requested for this spawn.
        {
            std::string startCwd = mOptions.cwd;
            if (startCwd.empty()) {
                if (const char* home = getenv("HOME"); home && *home) {
                    startCwd = home;
                } else if (const struct passwd* pw = getpwuid(getuid())) {
                    startCwd = pw->pw_dir;
                }
            }
            if (!startCwd.empty()) {
                if (chdir(startCwd.c_str()) == -1) {
                    spdlog::warn("Failed to chdir to '{}' -> {} {}", startCwd, errno, strerror(errno));
                }
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
    const int newW = this->width();
    const int newH = this->height();
    if (newW != oldW || newH != oldH) {
        mResizePending = true;
    }
    // Cascade cols into embeddeds — they span the parent's full width.
    // Rows stay pinned to whatever the applet sized them to. Fire the
    // platform-wired notifier so JS "resized" listeners see parent-
    // cascade resizes the same way they see explicit em.resize(rows).
    if (newW != oldW) {
        // Take mMutex briefly to snapshot (lineId, raw-pointer) pairs;
        // the parse worker (eviction callback) also takes mMutex, so
        // the map can't change under us. The embedded resize itself
        // takes the embedded's own mMutex — no nested same-mutex
        // acquire on this Terminal.
        std::vector<std::pair<uint64_t, Terminal*>> targets;
        {
            std::lock_guard<std::recursive_mutex> _lk(mutex());
            targets.reserve(mEmbedded.size());
            for (auto& [lineId, em] : mEmbedded)
                targets.emplace_back(lineId, em.get());
        }
        for (auto& [lineId, em] : targets) {
            int emRows = em->height();
            em->resize(newW, emRows);
            if (onEmbeddedResized) onEmbeddedResized(lineId, newW, emRows);
        }
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

void Terminal::markExited()
{
    if (mExited) return;
    mExited = true;
    // Stop further fd notifications. EV_EOF on a closed PTY stays set,
    // so kqueue/epoll would re-fire readFromFD on every run-loop pass
    // until onTick drains pendingExits and removePtyPoll runs — but
    // while kqueue is spinning, the run loop never reaches the
    // BeforeWaiting observer that triggers onTick, causing the hang.
    if (mLoop && mMasterFD >= 0) mLoop->removeFd(mMasterFD);
    if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
}

void Terminal::readFromFD()
{
    if (mExited) return;
    // Accumulate data into coalesce buffer without injecting. The
    // coalesce buffer is shared with the parse worker (via queueParse),
    // so take mReadBufferMutex while appending. We append in chunks
    // bounded by the local stack buf so the lock is held only briefly
    // even if the kernel has megabytes queued.
    //
    // Backpressure: when the coalesce buffer hits kReadBufferHigh we
    // disarm POLLIN on the master fd and return. The kernel PTY
    // buffer then fills and the child blocks on write(2). This caps
    // our memory use under a flooding producer and gives the parser
    // time to catch up. Resumed by the main-thread maybeResumeRead()
    // when the worker has drained the buffer below kReadBufferLow.
    char buf[65536];
    for (;;) {
        int ret;
        EINTRWRAP(ret, ::read(mMasterFD, buf, sizeof(buf)));
        if (ret == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno != EIO)
                spdlog::error("Failed to read from mMasterFD {} {}", errno, strerror(errno));
            markExited();
            return;
        } else if (ret == 0) {
            markExited();
            return;
        }
        bool atHigh = false;
        {
            std::lock_guard<std::mutex> lk(mReadBufferMutex);
            mReadCoalesceBuffer.insert(mReadCoalesceBuffer.end(), buf, buf + ret);
            if (mReadCoalesceBuffer.size() >= kReadBufferHigh)
                atHigh = true;
        }
        if (atHigh && !mReadPaused) {
            mReadPaused = true;
            if (mLoop && mMasterFD >= 0)
                mLoop->updateFd(mMasterFD,
                    mWritePollActive
                        ? EventLoop::FdEvents::Writable
                        : static_cast<EventLoop::FdEvents>(0));
            return;
        }
    }
}

void Terminal::maybeResumeRead()
{
    if (!mReadPaused || mExited) return;
    size_t sz;
    {
        std::lock_guard<std::mutex> lk(mReadBufferMutex);
        sz = mReadCoalesceBuffer.size();
    }
    if (sz <= kReadBufferLow) {
        mReadPaused = false;
        if (mLoop && mMasterFD >= 0)
            mLoop->updateFd(mMasterFD,
                mWritePollActive
                    ? EventLoop::FdEvents::ReadWrite
                    : EventLoop::FdEvents::Readable);
    }
}

void Terminal::flushReadBuffer()
{
    // Synchronous path: drains the entire coalesce buffer on the calling
    // thread. Used by tests and headless paths. Production code uses
    // queueParse() to dispatch parsing onto a worker.
    std::vector<char> buf;
    {
        std::lock_guard<std::mutex> lk(mReadBufferMutex);
        if (mReadCoalesceBuffer.empty()) return;
        buf.swap(mReadCoalesceBuffer);
    }

    if (mPlatformCbs.shouldFilterOutput && mPlatformCbs.shouldFilterOutput()) {
        std::string s(buf.begin(), buf.end());
        mPlatformCbs.filterOutput(s);
        if (!s.empty())
            injectData(s.data(), s.size());
    } else {
        injectData(buf.data(), buf.size());
    }

    // Check for foreground process change
    if (callbacks().onForegroundProcessChanged) {
        pid_t pgid = tcgetpgrp(mMasterFD);
        if (pgid > 0 && pgid != mLastFgPgid.load(std::memory_order_relaxed)) {
            mLastFgPgid.store(pgid, std::memory_order_relaxed);
            callbacks().onForegroundProcessChanged(foregroundProcess());
        }
    }
}

bool Terminal::queueParse(const ParseSubmitFn& submit)
{
    // Cheap empty-check without taking the buffer lock. False negatives
    // here are fine — readFromFD will trigger the next queueParse().
    if (mReadCoalesceBuffer.empty() && mParseInFlight.load(std::memory_order_acquire) == 0)
        return false;

    // Single-flight guard: if a parse is already queued/running, the
    // worker will loop back and pick up whatever's in the coalesce
    // buffer at the end of its current batch.
    int expected = 0;
    if (!mParseInFlight.compare_exchange_strong(expected, 1,
                                                std::memory_order_acq_rel)) {
        return false;
    }

    submit([this] {
        // Worker thread. Two nested loops:
        //
        //   outer: drain mReadCoalesceBuffer until it stays empty
        //          across one coalesce window.
        //   inner: parse the swapped buffer in budgeted chunks,
        //          releasing mMutex between chunks so other threads
        //          (renderer, main-thread one-off readers, the next
        //          tick's onTick path) can acquire it.
        //
        // The coalesce wait at the top of the outer loop matches
        // wezterm's mux_output_parser_coalesce_delay_ms (3 ms): we
        // wait briefly for more bytes to accumulate before grabbing
        // mMutex, so a flooded producer's lock-acquisition rate is
        // bounded to ~1/3ms ≈ 330 Hz per Terminal regardless of the
        // raw byte rate.
        constexpr auto kCoalesceWindow = std::chrono::milliseconds(3);
        // Per-chunk byte budget. Tuned for a parse-time budget of
        // roughly 1 ms on plain text at the codebase's measured
        // ~3.5 MB/s parser throughput, so a chunk holds the lock
        // for approximately one millisecond before yielding.
        constexpr size_t kParseChunkBudget = 4096;

        bool firstIteration = true;
        for (;;) {
            // Coalesce: nap briefly so newly-arrived bytes (from the
            // main-thread readFromFD callback) can pile up before we
            // grab the lock. First iteration skips the nap so initial
            // latency stays low for small writes — only subsequent
            // iterations (which only happen under sustained input)
            // pay the coalesce delay.
            if (!firstIteration) std::this_thread::sleep_for(kCoalesceWindow);
            firstIteration = false;

            std::vector<char> buf;
            {
                std::lock_guard<std::mutex> lk(mReadBufferMutex);
                if (mReadCoalesceBuffer.empty()) {
                    // Clear in-flight under the buffer lock so any
                    // concurrent readFromFD that appends after this
                    // point will be picked up by the next queueParse()
                    // on the main thread.
                    mParseInFlight.store(0, std::memory_order_release);
                    return;
                }
                buf.swap(mReadCoalesceBuffer);
            }

            // Filter pass (one-shot — runs on whatever filter result
            // is current; runOnMain bounce inside makes it safe).
            std::string filtered;
            const char* data;
            size_t size;
            if (mPlatformCbs.shouldFilterOutput && mPlatformCbs.shouldFilterOutput()) {
                filtered.assign(buf.begin(), buf.end());
                mPlatformCbs.filterOutput(filtered);
                data = filtered.data();
                size = filtered.size();
            } else {
                data = buf.data();
                size = buf.size();
            }

            // Apply the whole batch in one injectData call. Holding
            // mMutex for the full apply (rather than chunking with
            // mid-apply releases) matches wezterm's pattern: the
            // outer 3 ms coalesce is what bounds how often we
            // acquire mMutex; once acquired, we run to completion.
            // Mid-apply chunking caused observed waiter starvation
            // because std::recursive_mutex on Linux has no fairness
            // guarantee — the parser would re-acquire faster than a
            // separate-CPU waiter could wake. With one-shot apply,
            // a waiter sees mMutex free during the entire 3 ms
            // outer coalesce window.
            const uint64_t bt0 = obs::now_us();
            (void)injectData(data, size); // budget=0 → drain entire buffer
            if (auto dt = obs::now_us() - bt0; dt > 5000)
                spdlog::warn("[TIMING] queueParse: {} bytes in {} us",
                             size, dt);

            // Foreground process change check. tcgetpgrp + the
            // foregroundProcess() lookup touch the kernel and /proc; OK
            // off the main thread. The deferred callback in
            // buildTerminalCallbacks already posts to the event loop
            // for any onForegroundProcessChanged side-effects.
            if (callbacks().onForegroundProcessChanged && mMasterFD >= 0) {
                pid_t pgid = tcgetpgrp(mMasterFD);
                if (pgid > 0 && pgid != mLastFgPgid.load(std::memory_order_relaxed)) {
                    mLastFgPgid.store(pgid, std::memory_order_relaxed);
                    callbacks().onForegroundProcessChanged(foregroundProcess());
                }
            }
        }
    });
    return true;
}

void Terminal::writeToPTY(const char* data, size_t len)
{
    // Try to write directly first
    while (len > 0 && mWriteQueue.empty()) {
        int ret;
        EINTRWRAP(ret, ::write(mMasterFD, data, len));
        if (ret == -1) {
            // Kernel PTY buffer full: fall through to queue the rest so
            // we pick up where we left off on the next Writable poll. A
            // `return` here (prior bug) silently dropped bytes mid-paste
            // — which manifested as partial pastes (bracketed-paste end
            // marker arriving early to apps like Claude Code).
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // EIO = slave end closed (child process exited). Treat any
            // unrecoverable write error as terminal exit, matching the
            // readFromFD path. Drop remaining bytes; markExited unwatches
            // the fd and fires onTerminalExited once.
            if (errno != EIO)
                spdlog::error("Failed to write to master {} {}", errno, strerror(errno));
            markExited();
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
            if (errno != EIO)
                spdlog::error("Failed to write to master {} {}", errno, strerror(errno));
            markExited();
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

// ---------------------------------------------------------------------------
// Pixel rect / resize to fit
// ---------------------------------------------------------------------------

void Terminal::resizeToRect(float charW, float lineH,
                            float padL, float padT, float padR, float padB)
{
    if (mRect.isEmpty()) return;
    float usableW = std::max(0.0f, static_cast<float>(mRect.w) - padL - padR);
    float usableH = std::max(0.0f, static_cast<float>(mRect.h) - padT - padB);
    int cols = std::max(1, static_cast<int>(usableW / charW));
    int rows = std::max(1, static_cast<int>(usableH / lineH));
    resize(cols, rows);
}

// ---------------------------------------------------------------------------
// Popup children
// ---------------------------------------------------------------------------

Terminal* Terminal::createPopup(const std::string& id, int x, int y, int w, int h,
                                PlatformCallbacks pcbs)
{
    if (findPopup(id)) {
        spdlog::warn("Terminal: popup '{}' already exists", id);
        return nullptr;
    }

    TerminalCallbacks cbs;
    cbs.event = [this](TerminalEmulator*, int, void*) {
        if (onPopupEvent) onPopupEvent();
    };

    auto popup = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    TerminalOptions opts;
    opts.scrollbackLines = 0;
    popup->initHeadless(opts);
    popup->resize(w, h);
    popup->setCellPosition(x, y, w, h);

    spdlog::info("Terminal: created popup '{}' at ({},{}) {}x{}", id, x, y, w, h);
    // Popups have no tree nodeId — they're identified by the popupId string
    // relative to their parent Terminal.
    popup->setPopupId(id);
    mPopups.push_back(std::move(popup));
    return mPopups.back().get();
}

std::unique_ptr<Terminal> Terminal::extractPopup(const std::string& id)
{
    auto it = std::find_if(mPopups.begin(), mPopups.end(),
                           [&id](const std::unique_ptr<Terminal>& p) {
                               return p->popupId() == id;
                           });
    if (it == mPopups.end()) return nullptr;
    auto popup = std::move(*it);
    mPopups.erase(it);
    if (mFocusedPopupId == id) mFocusedPopupId.clear();
    spdlog::info("Terminal: extracted popup '{}'", id);
    return popup;
}

bool Terminal::resizePopup(const std::string& id, int x, int y, int w, int h)
{
    auto* p = findPopup(id);
    if (!p) return false;
    p->setCellPosition(x, y, w, h);
    p->resize(w, h);
    return true;
}

Terminal* Terminal::findPopup(const std::string& id)
{
    for (auto& p : mPopups)
        if (p->popupId() == id) return p.get();
    return nullptr;
}

bool Terminal::isCellCoveredByPopup(int col, int row) const
{
    for (const auto& p : mPopups) {
        if (col >= p->cellX() && col < p->cellX() + p->cellW() &&
            row >= p->cellY() && row < p->cellY() + p->cellH()) {
            return true;
        }
    }
    return false;
}

Terminal* Terminal::focusedPopup()
{
    if (mFocusedPopupId.empty()) return nullptr;
    return findPopup(mFocusedPopupId);
}

// ---------------------------------------------------------------------------
// Embedded children — document-anchored headless terminals
// ---------------------------------------------------------------------------

Terminal* Terminal::createEmbedded(int rows, PlatformCallbacks pcbs)
{
    if (rows <= 0) return nullptr;
    // Alt-screen mode has no persistent scrollback — embedded has nowhere to
    // anchor. Applets must wait until the shell returns to main screen.
    if (usingAltScreen()) return nullptr;
    // The anchor row is covered by the embedded and rows below are displaced
    // by (rows-1). Plus we advance one parent row for the cursor below the
    // embedded — that landing row is itself displaced. So we need `rows`
    // viewport rows of room beneath the cursor's logical position; if the
    // embedded plus its trailing cursor row can't fit at all, refuse.
    if (rows >= height()) return nullptr;

    // If the cursor is too close to the bottom of the viewport, the embedded
    // would extend past the visible area and the user couldn't scroll down to
    // see the rest (the live grid is already at the document tail). Push the
    // top of the document into history far enough that the future anchor sits
    // at viewport row <= height()-1-rows, leaving exactly one row below the
    // embedded for the displaced cursor.
    scrollCursorUpToFitBelow(rows);

    // mMutex covers the cursor read, the duplicate-check, the map
    // mutation, and the trailing injectData. Recursive lock — the
    // injectData re-acquires fine.
    std::lock_guard<std::recursive_mutex> _lk(mutex());

    // Current cursor row → anchor lineId. historySize() + cursorY() is the
    // absolute row of the current cursor position on main screen.
    const int anchorAbsRow = document().historySize() + cursorY();
    const uint64_t anchorLineId = document().lineIdForAbs(anchorAbsRow);
    if (anchorLineId == 0) return nullptr;
    if (mEmbedded.count(anchorLineId)) return nullptr;

    TerminalCallbacks cbs;
    cbs.event = [this](TerminalEmulator*, int, void*) {
        if (onPopupEvent) onPopupEvent();
    };

    auto embedded = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    TerminalOptions opts;
    opts.scrollbackLines = 0;
    embedded->initHeadless(opts);
    embedded->resize(width() > 0 ? width() : 80, rows);

    Terminal* raw = embedded.get();
    mEmbedded[anchorLineId] = std::move(embedded);

    // Advance the parent cursor to a fresh row so subsequent parent writes
    // don't target the (now-hidden) anchor row. CR+LF goes through the
    // emulator's standard path, creating a new tier-1 row below and
    // scrolling if we were at the last viewport row.
    injectData("\r\n", 2);

    spdlog::info("Terminal: created embedded at lineId={} rows={}", anchorLineId, rows);
    return raw;
}

std::unique_ptr<Terminal> Terminal::extractEmbedded(uint64_t lineId)
{
    std::unique_ptr<Terminal> t;
    {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        auto it = mEmbedded.find(lineId);
        if (it == mEmbedded.end()) return nullptr;
        t = std::move(it->second);
        mEmbedded.erase(it);
    }
    if (mFocusedEmbeddedLineId.load(std::memory_order_acquire) == lineId)
        mFocusedEmbeddedLineId.store(0, std::memory_order_release);
    spdlog::info("Terminal: extracted embedded lineId={}", lineId);
    return t;
}

bool Terminal::resizeEmbedded(uint64_t lineId, int rows)
{
    if (rows <= 0) return false;
    // Find the embedded under mMutex briefly, then resize outside
    // (resize() takes the embedded's own mutex, not this Terminal's).
    Terminal* em = nullptr;
    {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        auto it = mEmbedded.find(lineId);
        if (it == mEmbedded.end()) return false;
        em = it->second.get();
    }
    em->resize(width() > 0 ? width() : 80, rows);
    return true;
}

Terminal* Terminal::findEmbedded(uint64_t lineId)
{
    // One-off main-thread lookup — take mMutex briefly. The pointer
    // is valid until the entry is extracted/evicted (both go through
    // the graveyard, so even after caller releases the lock, the
    // pointer remains valid for at least one render frame).
    std::lock_guard<std::recursive_mutex> _lk(mutex());
    auto it = mEmbedded.find(lineId);
    return it == mEmbedded.end() ? nullptr : it->second.get();
}

Terminal* Terminal::focusedEmbedded()
{
    uint64_t id = mFocusedEmbeddedLineId.load(std::memory_order_acquire);
    if (id == 0) return nullptr;
    return findEmbedded(id);
}

TerminalEmulator* Terminal::activeTerm()
{
    if (auto* pp = focusedPopup())
        return pp;
    if (auto* em = focusedEmbedded())
        return em;
    return this;
}

void Terminal::onFullReset()
{
    // Called from inside injectData (parse worker, mMutex held). RIS
    // wipes scrollback / line ids, so anchored embeddeds become
    // orphans. Move them onto the eviction queue; main-thread
    // drainEvictedEmbeddeds() does the render-shadow mirror +
    // graveyard defer + JS "destroyed" delivery.
    if (mEmbedded.empty()) return;
    for (auto& [lineId, term] : mEmbedded) {
        mEvictedEmbeddeds.push_back({lineId, std::move(term)});
    }
    mEmbedded.clear();
    mFocusedEmbeddedLineId.store(0, std::memory_order_release);
    mEvictedHasItems.store(true, std::memory_order_release);
    if (onPopupEvent) onPopupEvent();
}

void Terminal::collectEmbeddedAnchors(std::vector<EmbeddedAnchor>& out) const
{
    if (usingAltScreen()) return; // hidden while in alt
    // Caller (TerminalSnapshot::update / liveSegmentHitTest) already
    // holds mMutex, so direct map iteration is race-free.
    for (const auto& [lineId, em] : mEmbedded) {
        out.push_back({lineId, em->height()});
    }
}

bool Terminal::liveSegmentHitTest(double cellRelX, double cellRelY,
                                  float cellWidth, float lineHeight,
                                  uint64_t& outLineId,
                                  int& outRelCol, int& outRelRow,
                                  int& outRelPixelX, int& outRelPixelY) const
{
    if (usingAltScreen() || !hasEmbeddeds() ||
        cellWidth <= 0.0f || lineHeight <= 0.0f)
        return false;

    // The main-thread hit-test consumes the same visible-anchor list the
    // render-thread snapshot builds. Can't read snap.segments from main
    // thread without racing snap.update(), so we recompute from live
    // state — the helper shares the build logic so both paths agree on
    // which anchors are visible and in what order. Lock mMutex for the
    // viewport + document + embedded-map reads.
    std::lock_guard<std::recursive_mutex> _lk(mutex());
    auto hits = collectVisibleAnchors(*this, viewportOffset(), height());
    if (hits.empty()) return false;

    int cumShiftRows = 0;
    for (const auto& h : hits) {
        double visualYStartPx = (h.viewRow + cumShiftRows) * lineHeight;
        double visualYEndPx   = visualYStartPx + h.rows * lineHeight;
        if (cellRelY >= visualYStartPx && cellRelY < visualYEndPx) {
            outLineId      = h.lineId;
            outRelPixelX   = static_cast<int>(cellRelX);
            outRelPixelY   = static_cast<int>(cellRelY - visualYStartPx);
            outRelCol      = static_cast<int>(cellRelX / cellWidth);
            outRelRow      = static_cast<int>((cellRelY - visualYStartPx) / lineHeight);
            return true;
        }
        cumShiftRows += (h.rows - 1);
    }
    return false;
}
