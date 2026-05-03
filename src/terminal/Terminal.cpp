#include "Terminal.h"
#include "Utils.h"
#include <chrono>
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
    // Order matters: drop the mux's read subscription FIRST so its
    // callback's captured `this` pointer can't fire after we've
    // started destructing. removeSync blocks until any in-flight
    // callback returns.
    //
    // Normal pane-close path goes through PlatformDawn::removePtyPoll
    // before reaching the destructor, so these are usually no-ops.
    // Defensive for tests / headless / mEvictedEmbeddeds graveyard
    // paths that might bypass removePtyPoll.
    if (mPtyMux && mMasterFD >= 0) mPtyMux->remove(mMasterFD);
    if (mWritePollActive && mEventLoop && mMasterFD >= 0) {
        mEventLoop->removeFd(mMasterFD);
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
    // Single-shot. CAS so concurrent callers (PtyMux thread on EOF
    // vs main on write failure) can't both proceed.
    bool already = mExited.exchange(true, std::memory_order_acq_rel);
    if (already) return;

    // Drop the mux's read subscription. From the mux thread (HUP
    // path) we MUST use removeAsync — removeSync would deadlock
    // because we ARE the polling thread. From main (write fail)
    // either would work, but removeAsync is uniform and correct in
    // both cases; the destructor's removeSync is the synchronous
    // fence we need before close(fd).
    if (mPtyMux && mMasterFD >= 0) mPtyMux->removeAsync(mMasterFD);

    // Drop the lazy POLLOUT registration if any. EventLoop is main-
    // thread-only — bounce through post() so this is safe to call
    // from the mux thread. The lambda runs on main and takes
    // mWriteQueueMutex to read/write mWritePollActive consistently
    // with writeToPTY / flushWriteQueue, which can be running on
    // worker for OSC replies right up until mExited propagates.
    if (mEventLoop && mMasterFD >= 0) {
        int fd = mMasterFD;
        EventLoop* loop = mEventLoop;
        loop->post([this, loop, fd]() {
            std::lock_guard<std::mutex> _lk(mWriteQueueMutex);
            if (mWritePollActive) {
                loop->removeFd(fd);
                mWritePollActive = false;
            }
        });
    }

    // onTerminalExited's body (Platform_Tabs.cpp) is
    // renderThread_->enqueueTerminalExit(t), which is itself thread-
    // safe (RenderThread::enqueueTerminalExit takes a mutex + wakes
    // the loop). So invoke directly from whichever thread we're on.
    if (mPlatformCbs.onTerminalExited) mPlatformCbs.onTerminalExited(this);
}

void Terminal::readFromFD()
{
    // Runs on the PtyMux thread. After markExited the mux callback
    // is being torn down asynchronously; if a stale fire still hits
    // us, just drop it.
    if (mExited.load(std::memory_order_acquire)) return;
    // Accumulate data into the coalesce buffer without injecting.
    // The coalesce buffer is shared with the parse worker (via
    // queueParse), so take mReadBufferMutex while appending. We
    // append in chunks bounded by the local stack buf so the lock
    // is held only briefly even if the kernel has megabytes queued.
    //
    // Backpressure: when the coalesce buffer hits kReadBufferHigh
    // we disarm POLLIN on the PtyMux poller and return. The kernel
    // PTY buffer then fills and the child blocks on write(2). This
    // caps our memory use under a flooding producer and gives the
    // parser time to catch up. Resumed by the worker thread's
    // maybeResumeRead() once the buffer drops below kReadBufferLow.
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
        if (atHigh && !mReadPaused.load(std::memory_order_acquire)) {
            mReadPaused.store(true, std::memory_order_release);
            // disable() called from the mux thread itself is in-
            // thread (no wakeup roundtrip); the change applies to
            // kqueue/epoll on this same iteration before we sleep
            // again.
            if (mPtyMux && mMasterFD >= 0) mPtyMux->disable(mMasterFD);
            return;
        }
    }
}

void Terminal::maybeResumeRead()
{
    // Runs on the parse worker thread (called from inside the
    // queueParse loop after each injectData batch).
    if (!mReadPaused.load(std::memory_order_acquire)) return;
    if (mExited.load(std::memory_order_acquire)) return;
    size_t sz;
    {
        std::lock_guard<std::mutex> lk(mReadBufferMutex);
        sz = mReadCoalesceBuffer.size();
    }
    if (sz <= kReadBufferLow) {
        // Clear the pause flag with CAS so we don't double-enable if
        // the mux thread happens to flip it back to true between our
        // load and store (it can't right now — mux only sets true
        // and only the worker clears — but CAS is cheap and makes
        // the invariant explicit).
        bool expected = true;
        if (mReadPaused.compare_exchange_strong(expected, false,
                                                std::memory_order_acq_rel)) {
            if (mPtyMux && mMasterFD >= 0) mPtyMux->enable(mMasterFD);
        }
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

    // Check for foreground process change. Both tcgetpgrp and proc_pidpath
    // are syscalls; tryRefreshForegroundProcess rate-limits and updates the
    // cached value that all readers (render thread, tab title, JS) consume.
    if (callbacks().onForegroundProcessChanged && tryRefreshForegroundProcess()) {
        callbacks().onForegroundProcessChanged(foregroundProcess());
    }
}

bool Terminal::queueParse(const ParseSubmitFn& submit)
{
    // Buffer empty + no parse in flight = nothing to schedule. The
    // empty() read MUST hold mReadBufferMutex because the PtyMux
    // thread mutates mReadCoalesceBuffer (vector::insert) under
    // that lock; an unsynchronized read here is a TSan violation
    // even if the eventual answer would be a benign "false negative
    // → next readFromFD wakes us." Lock is microseconds-uncontended
    // (mux thread only holds it during a single insert).
    {
        std::lock_guard<std::mutex> lk(mReadBufferMutex);
        if (mReadCoalesceBuffer.empty()
            && mParseInFlight.load(std::memory_order_acquire) == 0)
            return false;
    }

    // Single-flight guard: if a parse is already queued/running, the
    // worker will loop back and pick up whatever's in the coalesce
    // buffer at the end of its current batch.
    int expected = 0;
    if (!mParseInFlight.compare_exchange_strong(expected, 1,
                                                std::memory_order_acq_rel)) {
        return false;
    }

    submit([this] {
        // Worker thread. Loops draining mReadCoalesceBuffer until it
        // stays empty across one coalesce window. Each iteration calls
        // injectData on the swapped buffer; injectData internally runs
        // parseToActions under mParseStateMutex (lock-free wrt mMutex)
        // and applyActions under mMutex.
        //
        // The coalesce wait at the top matches the 3 ms window: we
        // wait briefly for more bytes to accumulate before grabbing
        // the buffer, so a flooded producer's parse-acquisition rate
        // is bounded to ~1/3ms ≈ 330 Hz per Terminal regardless of
        // the raw byte rate.
        constexpr auto kCoalesceWindow = std::chrono::milliseconds(3);

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

            // Apply the whole batch in one injectData call. The 3 ms
            // outer coalesce bounds how often we acquire mMutex;
            // once acquired (inside injectData::applyActions), we run
            // to completion. parseToActions runs without mMutex —
            // render thread can read concurrently with decode.
            (void)injectData(data, size);

            // Read backpressure rearm: now that we've drained a
            // batch, check whether we should re-enable POLLIN on
            // the PtyMux poller. This used to live in the main
            // thread's onTick but moved here so backpressure is
            // independent of main-loop tick rate (DEC 2026 sync
            // blocks otherwise stalled the writer indefinitely).
            maybeResumeRead();

            // Foreground process change check. Rate-limited at the source
            // by tryRefreshForegroundProcess (200 ms); updates the cached
            // value all readers consume (render frame builder, tab title,
            // JS API) so they don't each issue their own ioctl per use.
            if (callbacks().onForegroundProcessChanged && mMasterFD >= 0
                && tryRefreshForegroundProcess()) {
                callbacks().onForegroundProcessChanged(foregroundProcess());
            }
        }
    });
    return true;
}

void Terminal::writeToPTY(const char* data, size_t len)
{
    // Callable from main (keystrokes / paste / writeText) AND from
    // the parse worker (OSC/DA replies through writeToOutput).
    // mWriteQueueMutex serializes both. After markExited the fd is
    // on its way out; bail before issuing a syscall that would hit
    // EIO on a half-torn-down PTY.
    if (mExited.load(std::memory_order_acquire)) return;

    bool exitOnError = false;
    {
        std::lock_guard<std::mutex> _lk(mWriteQueueMutex);

        // Try to write directly first
        while (len > 0 && mWriteQueue.empty()) {
            int ret;
            EINTRWRAP(ret, ::write(mMasterFD, data, len));
            if (ret == -1) {
                // Kernel PTY buffer full: fall through to queue the rest
                // so we pick up where we left off on the next Writable
                // poll. A `return` here (prior bug) silently dropped bytes
                // mid-paste — which manifested as partial pastes
                // (bracketed-paste end marker arriving early to apps
                // like Claude Code).
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                // EIO = slave end closed (child process exited). Treat
                // any unrecoverable write error as terminal exit. Drop
                // remaining bytes; markExited unwatches the fd and
                // fires onTerminalExited once. Defer the markExited
                // call until after we drop the lock to avoid taking
                // the EventLoop's post() lock under ours.
                if (errno != EIO)
                    spdlog::error("Failed to write to master {} {}", errno, strerror(errno));
                exitOnError = true;
                break;
            }
            data += ret;
            len -= ret;
        }

        // Queue remaining data and lazily register the fd with the
        // EventLoop for POLLOUT. Reads no longer go through EventLoop
        // (PtyMux owns them), so this watchFd subscribes to write only.
        if (!exitOnError && len > 0) {
            mWriteQueue.insert(mWriteQueue.end(), data, data + len);
            if (!mWritePollActive && mEventLoop && mMasterFD >= 0) {
                Terminal* self = this;
                mEventLoop->watchFd(mMasterFD, EventLoop::FdEvents::Writable,
                    [self](EventLoop::FdEvents ev) {
                        if (ev & EventLoop::FdEvents::Writable) self->flushWriteQueue();
                    });
                mWritePollActive = true;
            }
        }
    }

    if (exitOnError) markExited();
}

void Terminal::flushWriteQueue()
{
    if (mExited.load(std::memory_order_acquire)) return;

    bool exitOnError = false;
    {
        std::lock_guard<std::mutex> _lk(mWriteQueueMutex);
        while (!mWriteQueue.empty()) {
            int ret;
            EINTRWRAP(ret, ::write(mMasterFD, mWriteQueue.data(), mWriteQueue.size()));
            if (ret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return; // try again next poll
                if (errno != EIO)
                    spdlog::error("Failed to write to master {} {}", errno, strerror(errno));
                exitOnError = true;
                break;
            }
            mWriteQueue.erase(mWriteQueue.begin(), mWriteQueue.begin() + ret);
        }
        // Queue drained — drop the lazy POLLOUT registration entirely.
        if (!exitOnError && mWritePollActive && mEventLoop && mMasterFD >= 0) {
            mEventLoop->removeFd(mMasterFD);
            mWritePollActive = false;
        }
    }

    if (exitOnError) markExited();
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

bool Terminal::fgPollDue() const noexcept
{
    using clock = std::chrono::steady_clock;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock::now().time_since_epoch()).count();
    int64_t prev = mLastFgPollNs.load(std::memory_order_relaxed);
    if (prev != 0 && (now - prev) < kFgPollMinNs) return false;
    // Try to claim the slot. If another caller wins, skip — they'll do the
    // ioctl. We don't loop because the cost of "two pollers raced and both
    // skipped this tick" is at most one extra ~200ms of stale fg name; not
    // worth the CAS retry.
    return mLastFgPollNs.compare_exchange_strong(prev, now,
        std::memory_order_relaxed, std::memory_order_relaxed);
}

namespace {
std::string lookupFgProcessName(int masterFD)
{
    if (masterFD < 0) return {};
    pid_t pgid = tcgetpgrp(masterFD);
    if (pgid < 0) return {};

#ifdef __APPLE__
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pgid, pathbuf, sizeof(pathbuf)) > 0) {
        const char* slash = strrchr(pathbuf, '/');
        return slash ? slash + 1 : pathbuf;
    }
    return {};
#else
    char comm[256];
    snprintf(comm, sizeof(comm), "/proc/%d/comm", static_cast<int>(pgid));
    FILE* f = fopen(comm, "r");
    if (!f) return {};
    char name[256] = {};
    if (fgets(name, sizeof(name), f)) {
        size_t len = strlen(name);
        if (len > 0 && name[len - 1] == '\n') name[len - 1] = '\0';
    }
    fclose(f);
    return name;
#endif
}
} // namespace

bool Terminal::tryRefreshForegroundProcess()
{
    if (!fgPollDue()) return false;
    if (mMasterFD < 0) return false;

    pid_t pgid = tcgetpgrp(mMasterFD);
    if (pgid < 0) return false;

    pid_t prevPgid = mLastFgPgid.exchange(pgid, std::memory_order_relaxed);
    bool changed = (prevPgid != pgid);
    if (!changed) return false;

    std::string name = lookupFgProcessName(mMasterFD);
    {
        std::unique_lock lk(mFgCacheMutex);
        mFgCache = std::move(name);
    }
    return true;
}

std::string Terminal::foregroundProcess() const
{
    // Pure cache read. The actual ioctl + proc_pidpath lookup is gated
    // behind tryRefreshForegroundProcess, called from the parser worker
    // path at ~5 Hz. Render thread / tab title computation reads only.
    std::shared_lock lk(mFgCacheMutex);
    return mFgCache;
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
    // mutation, and the trailing applyControl calls (which require it).
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
    // don't target the (now-hidden) anchor row. Apply CR + LF directly
    // via applyControl rather than via injectData. Lock ordering for
    // injectData is mParseStateMutex first, then mMutex; we already
    // hold mMutex above, so calling injectData would acquire in the
    // wrong order and deadlock against any concurrent parse-worker
    // path.
    applyControl(ParserAction::ControlCode::CR);
    applyControl(ParserAction::ControlCode::LF);

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
