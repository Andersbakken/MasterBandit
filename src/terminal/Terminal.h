#pragma once
#include "PtyMux.h"
#include "Rect.h"
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
#include "Uuid.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio> // FILE* (output capture)
#include <eventloop/EventLoop.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Terminal; // forward declare for PlatformCallbacks

struct PlatformCallbacks
{
    std::function<void(Terminal *)> onTerminalExited;
    std::function<void()> quit;
    // Output filter: check shouldFilterOutput first (cheap, no copy),
    // then call filterOutput only if it returns true.
    std::function<bool()> shouldFilterOutput;
    std::function<void(std::string &data)> filterOutput;
    // Input filter: same pattern.
    std::function<bool()> shouldFilterInput;
    std::function<void(std::string &data)> filterInput;
    // Headless input sink: receives keystrokes when no PTY exists (applet mode).
    std::function<void(const char *data, size_t len)> onInput;
};

class Terminal : public TerminalEmulator
{
public:
    Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks);
    ~Terminal() override;

    // --- Identity ---
    // The Terminal's tree-node UUID is the sole identifier. Popups / overlays
    // that don't live in the tree have nil nodeId and key off their parent
    // plus popupId string.
    Uuid id() const { return mNodeId; }

    Uuid nodeId() const { return mNodeId; }

    void setNodeId(Uuid u) { mNodeId = u; }

    // --- PTY lifecycle ---
    bool init(const TerminalOptions &options);
    // Initialize without a PTY or child process. For script-driven applets.
    bool initHeadless(const TerminalOptions &options);
    // Initialize with a PTY pair but no child process — external apps open
    // ttyName() and read/write. We hold the slave fd open so the master
    // never sees EOF/EIO when writers close. Used by pty-backed popups.
    bool initPtyOnly(const TerminalOptions &options);

    void setEventLoop(EventLoop *loop);

    void setPtyMux(PtyMux *mux) { mPtyMux = mux; }

    int masterFD() const { return mMasterFD; }

    // Slave device path ("/dev/pts/N"); empty for headless terminals.
    const std::string &ttyName() const { return mTtyName; }

    bool isHeadless() const { return mHeadless; }

    void readFromFD();
    // Synchronous parse on the calling thread. Used by tests and by
    // headless code paths that don't have a worker pool. Production
    // PTY ticks should call queueParse() instead.
    void flushReadBuffer();

    // Asynchronous parse. Atomically swaps mReadCoalesceBuffer into a
    // worker-owned buffer and submits an injectData task to `submit`.
    // Caller passes the submission function (typically WorkerPool::submit
    // bound to PlatformDawn's pool). At most one parse task per Terminal
    // is in flight at a time: if a task is already queued, queueParse
    // returns immediately and the in-flight worker will pick up any
    // newly-arrived bytes when it loops at the end of its current batch.
    // Returns true iff a new task was submitted (caller can use this to
    // skip wakeups, etc.).
    using ParseSubmitFn = std::function<void(std::function<void()>)>;
    bool queueParse(const ParseSubmitFn &submit);

    // Stored variant of queueParse: PtyMux thread invokes this directly
    // after readFromFD appends bytes, so parse-trigger doesn't go through
    // main. Caller (PlatformDawn::addPtyPoll) wires the submit fn to the
    // worker pool. Headless terminals leave mParseSubmit empty and call
    // flushReadBuffer instead.
    void setParseSubmit(ParseSubmitFn fn) { mParseSubmit = std::move(fn); }

    bool queueParse() { return mParseSubmit ? queueParse(mParseSubmit) : false; }

    // True iff a parse task is queued or currently running on a worker.
    // Used by the graveyard to defer destruction until parsing finishes.
    bool parseInFlight() const
    {
        return mParseInFlight.load(std::memory_order_acquire) != 0;
    }

    // parseInFlight for this Terminal plus its popup children. Pane-level
    // graveyard sweeps use this so a worker parsing a pty-backed popup
    // can't race the pane destructor freeing mPopups.
    bool anyParseInFlight() const
    {
        if (parseInFlight()) {
            return true;
        }
        for (const auto &p : mPopups) {
            if (p->parseInFlight()) {
                return true;
            }
        }
        return false;
    }

    // Append bytes to the read coalesce buffer and schedule a parse —
    // same ordered byte stream as PTY reads, so script injects can't land
    // mid-escape-sequence between worker batches. Synchronous
    // flushReadBuffer fallback when no parse submit fn is wired.
    void enqueueParseBytes(const char *data, size_t len);

    void flushWriteQueue();
    // Paste: wraps in \x1b[200~/\x1b[201~ when DECSET 2004 is active on the
    // terminal. Use for real clipboard/selection pastes so the shell's paste
    // handling (quoting, auto-suggest suppression, etc.) takes effect.
    void pasteText(const std::string &text);

    // Write: raw send to the PTY, no bracketing. Use for synthetic keystrokes,
    // OSC responses, or anything that isn't semantically a user paste.
    void writeText(const std::string &text) { writeToPTY(text.data(), text.size()); }

    // Query the foreground process name via tcgetpgrp + platform process lookup
    std::string foregroundProcess() const;
    // Resolved executable path of the foreground process (spoof-resistant,
    // unlike the comm/argv0-derived name). Empty when unresolvable.
    std::string foregroundExe() const;
    void resize(int width, int height) override;

    // Deferred TIOCSWINSZ: set by resize(), consumed by flushPendingResize()
    bool hasResizePending() const { return mResizePending.load(std::memory_order_acquire); }

    void flushPendingResize();

    // --- Pixel rect (set by Engine::computeTabRects for panes) ---
    void setRect(const Rect &r) { mRect = r; }

    const Rect &rect() const { return mRect; }

    // Resize the terminal to fit current rect given font metrics
    void resizeToRect(float charW, float lineH,
                      float padL = 0, float padT = 0,
                      float padR = 0, float padB = 0);

    // --- Cell-coordinate position (for popup children / embedded terminals) ---
    int cellX() const { return mCellX; }

    int cellY() const { return mCellY; }

    int cellW() const { return mCellW; }

    int cellH() const { return mCellH; }

    void setCellPosition(int x, int y, int w, int h)
    {
        mCellX = x;
        mCellY = y;
        mCellW = w;
        mCellH = h;
    }

    // --- Popup string identifier (set on popup children) ---
    const std::string &popupId() const { return mPopupId; }

    void setPopupId(const std::string &id) { mPopupId = id; }

    // --- Title, icon, progress, CWD ---
    // title/icon are pull-model: reads the emulator's XTWINOPS stack.
    // nullopt means "no title in effect" — callers decide the fallback.
    // A JS-set custom title (mCustomTitle) wins over the OSC stack;
    // when set, OSC 0/2 writes still update the underlying stack so the
    // original title returns once the override is cleared.
    std::optional<std::string> title() const
    {
        if (mCustomTitle.has_value()) {
            return mCustomTitle;
        }
        return currentTitle();
    }

    // OSC-only title, ignoring any JS override. Internal callers that
    // need the "real" shell-supplied title use this.
    std::optional<std::string> oscTitle() const { return currentTitle(); }

    // JS-set per-pane title override. nullopt clears.
    void setCustomTitle(std::optional<std::string> t) { mCustomTitle = std::move(t); }

    const std::optional<std::string> &customTitle() const { return mCustomTitle; }

    std::optional<std::string> icon() const { return currentIcon(); }

    int progressState() const { return mProgressState; }

    int progressPct() const { return mProgressPct; }

    void setProgress(int state, int pct)
    {
        mProgressState = state;
        mProgressPct   = pct;
    }

    const std::string &cwd() const { return mCWD; }

    void setCWD(const std::string &d) { mCWD = d; }

    // --- Popup children ---
    // Create a child terminal at cell coordinates. Headless by default;
    // pty=true backs it with a forkless PTY pair (see initPtyOnly).
    Terminal *createPopup(const std::string &id, int x, int y, int w, int h,
                          PlatformCallbacks pcbs, bool pty = false);
    // Move a popup out for deferred destruction. Returns the moved Terminal
    // on success, or nullptr if not found.
    std::unique_ptr<Terminal> extractPopup(const std::string &id);
    bool resizePopup(const std::string &id, int x, int y, int w, int h);
    Terminal *findPopup(const std::string &id);

    const std::vector<std::unique_ptr<Terminal>> &popups() const { return mPopups; }

    // True if (col, row) lies inside any popup child's cell rect.
    bool isCellCoveredByPopup(int col, int row) const;

    std::string focusedPopupId() const { return mFocusedPopupId; }

    void setFocusedPopup(const std::string &id) { mFocusedPopupId = id; }

    void clearFocusedPopup() { mFocusedPopupId.clear(); }

    Terminal *focusedPopup();

    // --- Embedded terminals (document-anchored inline surfaces) ---
    // Headless sibling of popups but anchored to a stable logical-line id
    // instead of viewport cell coordinates — the embedded scrolls with the
    // document and is destroyed when its anchor row evicts past the archive
    // cap. The anchor row is visually covered by the embedded; subsequent
    // rows are pushed down by (rows-1) cell heights (Model B displacement).
    //
    // Creation captures the parent's current cursor row as the anchor lineId,
    // then advances the parent cursor to a new row below so subsequent parent
    // writes don't target the now-hidden anchor cells. Fails (returns nullptr)
    // when parent is on alt-screen, when `rows` <= 0, or when the cursor row
    // already has an embedded attached.
    Terminal *createEmbedded(int rows, PlatformCallbacks pcbs);
    // Move an embedded out for deferred destruction. Returns the moved
    // Terminal on success, or nullptr if no embedded is anchored at lineId.
    std::unique_ptr<Terminal> extractEmbedded(uint64_t lineId);
    // Resize an existing embedded. Only rows change; cols track parent cols.
    bool resizeEmbedded(uint64_t lineId, int rows);
    Terminal *findEmbedded(uint64_t lineId);

    // Embedded-map iteration helpers. All acquire mMutex (which the
    // parse worker also holds during its apply phase). Use these
    // instead of exposing the map directly so callers can't forget
    // the lock.
    template <typename Fn>
    void forEachEmbedded(Fn &&fn) const
    {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        for (const auto &[lineId, em] : mEmbedded) {
            fn(lineId, *em);
        }
    }

    bool hasEmbeddeds() const
    {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        return !mEmbedded.empty();
    }

    size_t embeddedCount() const
    {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        return mEmbedded.size();
    }

    // Atomic: written by the parse-worker eviction callback (clears to 0
    // when the focused embedded's anchor evicts) and by main-thread
    // create/destroy/focus paths. Read on the per-tick render path.
    uint64_t focusedEmbeddedLineId() const { return mFocusedEmbeddedLineId.load(std::memory_order_acquire); }

    void setFocusedEmbeddedLineId(uint64_t id) { mFocusedEmbeddedLineId.store(id, std::memory_order_release); }

    void clearFocusedEmbedded() { mFocusedEmbeddedLineId.store(0, std::memory_order_release); }

    Terminal *focusedEmbedded();

    // Returns the focused popup's emulator if one exists, else the focused
    // embedded's emulator if any, else this.
    TerminalEmulator *activeTerm();

    // Hook consumed by TerminalSnapshot::update() to build the visual-layout
    // segment list. Pushes (lineId, rowCount) for every embedded currently
    // attached to this Terminal. Alt-screen hides embeddeds so the list
    // stays empty there.
    void collectEmbeddedAnchors(std::vector<EmbeddedAnchor> &out) const override;

    // RIS hand-off: move every embedded onto the eviction queue so the
    // platform main-thread tick tears them down via the same render-shadow
    // mirror + graveyard path the line-id eviction callback uses.
    void onFullReset() override;

    // Input-path hit-test. Returns true if viewport-pixel-(X,Y) (pane-
    // content-relative, i.e. after padLeft / padTop subtraction) falls
    // inside a visible embedded's displaced band. Walks live state —
    // InputController can't read the render thread's
    // TerminalSnapshot.segments from the main thread without racing
    // snapshot.update(). `cellWidth` / `lineHeight` = pixel dimensions of
    // one cell. On hit, outRelCol / outRelRow are the embedded-local cell
    // coordinates, and outRelPixelX / outRelPixelY are the embedded-local
    // pixel offsets.
    bool liveSegmentHitTest(double cellRelX, double cellRelY,
                            float cellWidth, float lineHeight,
                            uint64_t &outLineId,
                            int &outRelCol, int &outRelRow,
                            int &outRelPixelX, int &outRelPixelY) const;

    // Set by platform; called when a popup or embedded emulator fires an event (triggers redraw)
    std::function<void()> onPopupEvent;

    // Set by platform; called when an embedded gets resized as a side
    // effect of the parent pane's cols changing. Fires after the resize
    // so the callback can read `em->width()/height()` directly. Platform
    // wires this to scriptEngine_.deliverEmbeddedResized so the JS
    // "resized" event fires uniformly whether the resize was initiated
    // by JS (em.resize) or cascaded from the parent.
    std::function<void(uint64_t lineId, int cols, int rows)> onEmbeddedResized;

    // Set by platform; called when a logical-line id is evicted from
    // scrollback past the archive cap. Fired from the parse worker
    // thread under mMutex (delegated from Document::setOnLineIdEvicted),
    // so callbacks must NOT take locks the worker may hold and must NOT
    // call back into the engine synchronously — post to the main thread.
    // Platform wires this to scriptEngine_.notifyRowEvicted so JS
    // "rowEvicted" listeners fire after the parser releases the lock.
    // Invoked AFTER the existing internal embedded-cleanup logic for
    // the same lineId, so listeners observe a consistent post-eviction
    // state (the embedded, if any, has already been graveyarded).
    std::function<void(uint64_t lineId)> onLineIdEvicted;

    // ─────────────────────────────────────────────────────────────────
    // Output-capture taps
    //
    // A "capture" subscribes to the post-coalesce, pre-filter PTY
    // output stream and writes it to a file. Multiple captures per
    // Terminal are supported — different consumers (LLM tool, debug
    // log, asciicast recorder) can attach concurrently. Uniqueness
    // is enforced by file path: addOutputCapture refuses duplicates
    // pointing at the same path so two callers can't unknowingly
    // interleave writes into the same file.
    //
    // THREADING: producer side fires from any thread that consumed
    // bytes from mReadCoalesceBuffer (main `flushReadBuffer` and
    // worker `queueParse` lambda). Each capture serialises its own
    // file writes under a per-capture mutex. The captures vector
    // itself is protected by mCapturesMutex; producers grab it
    // briefly to snapshot the vector, then release before doing I/O.
    // I/O is done while holding the per-capture mutex — slow disk
    // writes block whichever thread is delivering, by design (see
    // the design conversation; queue + dedicated writer thread is a
    // documented follow-up if this shows up in profiles).
    //
    // Captures auto-stop on write failure. Stop also fires the
    // onCaptureStopped callback (set by platform → scriptEngine_)
    // so JS gets a `stopped` event with a structured reason.
    // On-disk capture format.
    //   Raw       : every byte the emulator received, verbatim. Includes
    //               all escapes and binary bytes. Smallest / cheapest.
    //   Asciicast : asciicast v2 JSON-Lines, replayable with the
    //               asciinema toolchain.
    //   Text      : escape-stripped plain text, intended for LLM
    //               consumption or human reading without an interpreter.
    //               ANSI/CSI/OSC/DCS/SS3 sequences and most C0 controls
    //               are dropped; printable ASCII, UTF-8 high bytes,
    //               TAB, and LF are preserved. CR is normalised:
    //               "\r\n" → "\n", bare "\r" dropped (in-place updates
    //               like progress bars produce repeated content but no
    //               cursor-driven overwrites — modelling overwrites
    //               would require a full emulator and isn't worth it
    //               for a follow-along log).
    enum class CaptureFormat
    {
        Raw,
        Asciicast,
        Text
    };
    enum class CaptureStopReason
    {
        // External: script called .stop() or the owning instance was
        // unloaded. No "error" payload accompanies this.
        Explicit,
        // Internal: a write to the destination file failed (disk full,
        // EIO, fd revoked, etc.). The captured errno-style message is
        // surfaced as the event payload.
        IoError,
    };

    // Caller-supplied callback for the JS "stopped" event. Payload is
    // (path, reason, errorMessage). errorMessage is empty for Explicit
    // stops. Fired from whichever thread observed the stop — platform
    // is responsible for hopping to main if it needs JS-thread context.
    // Set by Platform_Tabs.cpp at Terminal construction.
    std::function<void(const std::string &path,
                       CaptureStopReason reason,
                       const std::string &errorMessage)>
        onCaptureStopped;

    // Open a new output capture targeting `path`. Returns true on
    // success (file opened, capture registered), false if `path` is
    // already capturing on this Terminal or if the file open failed.
    // For asciicast format, writes the v2 header line synchronously
    // so the destination file is a valid asciicast even if no PTY
    // bytes ever arrive. cols/rows/title come from the live emulator
    // state at registration time. errorOut (if non-null) carries a
    // human-readable failure reason on `false` return.
    bool addOutputCapture(const std::string &path,
                          CaptureFormat format,
                          std::string *errorOut = nullptr);

    // Stop a previously-added capture. Idempotent — unknown paths or
    // already-stopped captures return false silently. Fires the
    // onCaptureStopped callback with reason=Explicit on success.
    bool removeOutputCapture(const std::string &path);

    // True iff a capture is currently registered for `path` on this
    // Terminal. Cheap — takes mCapturesMutex briefly.
    bool hasOutputCapture(const std::string &path) const;

protected:
    void writeToOutput(const char *data, size_t len) override;

private:
    void writeToPTY(const char *data, size_t len);
    // Open the pty pair into mMasterFD/mSlaveFD and record mTtyName.
    bool openPty();
    PlatformCallbacks mPlatformCbs;
    TerminalOptions mOptions;
    Uuid mNodeId;
    int mMasterFD { -1 };
    // Open only for initPtyOnly terminals (held for the lifetime);
    // init() closes it in the parent after fork.
    int mSlaveFD { -1 };
    std::string mTtyName;
    bool mHeadless { false };
    std::atomic<bool> mResizePending { false };
    // Read/written from both the main thread (flushReadBuffer fallback)
    // and the parse worker (queueParse loop) in mutually exclusive
    // code paths today, but typed as atomic so the absence of a
    // formal happens-before is no longer UB and so future readers
    // (e.g. for stats) can sample without locks.
    std::atomic<pid_t> mLastFgPgid { -1 };
    // tcgetpgrp + proc_pidpath are kernel/ioctl traps. The foreground
    // process changes on the order of seconds (run a new command), but
    // without rate limiting the check fires per readFromFD wakeup AND
    // per parse-worker loop iteration — during a 1 MiB vtebench scroll
    // that's hundreds of ioctl(2)s for no reason. Sample at most once
    // every kFgPollMinNs nanoseconds. Atomic so the parser worker and
    // the main thread don't race on the timestamp.
    static constexpr int64_t kFgPollMinNs = 200'000'000; // 200 ms
    mutable std::atomic<int64_t> mLastFgPollNs { 0 };
    mutable std::shared_mutex mFgCacheMutex;
    mutable std::string mFgCache;
    mutable std::string mFgExeCache;
    // Rate-limited rejections strand pgid changes when no further bytes
    // arrive — e.g. a child process forks, prints once, then idles. The
    // worker can't catch up on the next iteration because there isn't
    // one. A blocked call instead arms a one-shot timer for ~kFgPollMinNs
    // later that retries the check. Any subsequent call (data-driven
    // refresh OR the timer firing) cancels the pending timer; if it was
    // also blocked, it re-arms a fresh one. Atomic flag is the
    // single-flight gate so a flood doesn't spam the event loop with
    // schedule/cancel posts. Timer id touched only on main.
    std::atomic<bool> mDeferredFgScheduled { false };
    EventLoop::TimerId mDeferredFgTimerId { 0 };
    enum class FgRefreshResult
    {
        Changed,
        Unchanged,
        RateLimited
    };
    bool fgPollDue() const noexcept;
    // Refresh the cached foreground process name + pgid if the rate
    // limiter allows. Returns Changed when callers should fire
    // onForegroundProcessChanged. Safe to call from any thread.
    FgRefreshResult tryRefreshForegroundProcess();
    // Run tryRefreshForegroundProcess, fire the callback on Changed,
    // arm the deferred-retry timer on RateLimited, cancel any pending
    // timer otherwise. Centralizes the policy so the worker iteration
    // and the timer callback share it.
    void pollAndNotifyForegroundProcess();
    void scheduleDeferredFgCheck();
    void cancelDeferredFgCheck();
    // Main-thread event loop — used for PTY *writes* only (lazy
    // POLLOUT registration in writeToPTY, removed in flushWriteQueue
    // when the queue drains) and for post()-ing main-thread work from
    // worker / mux threads.
    EventLoop *mEventLoop { nullptr };
    // Dedicated PTY-read multiplexer. Owns its own thread; readFromFD
    // runs on that thread (NOT main). Headless terminals leave this
    // null and use flushReadBuffer directly.
    PtyMux *mPtyMux { nullptr };
    // Submission function bound to PlatformDawn's worker pool. Invoked
    // by the PtyMux callback after readFromFD appends bytes — moves the
    // parse-trigger off main entirely.
    ParseSubmitFn mParseSubmit;
    EventLoop::TimerId mWritePollId { 0 };
    // Tracks whether the master fd is currently registered with
    // mEventLoop for POLLOUT. Mutated under mWriteQueueMutex from
    // writeToPTY / flushWriteQueue (main, or worker via the
    // writeToOutput → writeToPTY path used by the parser for
    // OSC/DA replies) and from a posted lambda fired by markExited.
    bool mWritePollActive { false };
    // Set once the PTY has delivered EOF or an unrecoverable I/O
    // error. CAS-then-act in markExited; read in writeToPTY /
    // flushWriteQueue / readFromFD / maybeResumeRead to short-circuit.
    // Atomic because markExited may run on the PtyMux thread (HUP
    // path) while writes happen on main / worker.
    std::atomic<bool> mExited { false };
    // Shell pid from fork(). Watched so the tab closes when the shell
    // process itself exits, even if a backgrounded child still holds
    // the slave PTY fd open (e.g. `foo &!; exit` in zsh). Without
    // this, mb only learns "shell exited" via PTY master EOF — which
    // requires every slave-side fd to close. Modern terminals (kitty,
    // foot, wezterm, alacritty, ghostty, iTerm2, Terminal.app) all
    // watch the shell pid for this reason. -1 in headless mode.
    pid_t mShellPid { -1 };
#ifdef __linux__
    // pidfd opened post-fork; epoll/EventLoop watches it as Readable.
    // Fires when the shell process exits. Tear down in destructor and
    // in markExited (in case PTY EOF wins the race).
    int mPidFd { -1 };
    // Shared "alive" gate captured by the pidfd watch lambda. Set to
    // false before the Terminal is torn down so a still-queued cb
    // (Remove is async — see FdPollerEpoll) can no-op instead of
    // dereferencing freed `this`.
    std::shared_ptr<std::atomic<bool>> mPidWatchAlive;
#endif
#ifdef __APPLE__
    // Detached helper thread blocks on waitpid(mShellPid) and post()s
    // markExited back to the EventLoop on exit. macOS's EventLoop API
    // doesn't expose EVFILT_PROC, so this is the portable workaround.
    //
    // Lifecycle (the hard part): the helper is blocked in waitpid and
    // can't be cancelled portably. If `Terminal` is destroyed before
    // the child dies (e.g. pane closed via the close button while the
    // shell still runs), we must NOT free state the helper reads.
    // Solution: a heap-allocated shared block owned by both the helper
    // and the Terminal (shared_ptr). The Terminal flips a "dead" flag
    // in the block on destruct; the helper reads it after waitpid
    // returns and bails without touching `this` if set.
    struct PidWatch
    {
        std::atomic<bool> ownerDead { false };
        EventLoop *loop { nullptr }; // captured snapshot; loop outlives Terminal
        pid_t pid { -1 };
        Terminal *owner { nullptr }; // only dereferenced when !ownerDead
    };

    std::shared_ptr<PidWatch> mPidWatch;
    std::thread mPidWatchThread; // detached on shutdown
#endif
    // Bytes queued for write but not yet accepted by the kernel
    // PTY input buffer. Serialized by mWriteQueueMutex because
    // writeToPTY can be invoked from main (keystrokes / paste /
    // writeText) and from the parse worker (OSC/DA responses via
    // TerminalEmulator::writeToOutput → Terminal::writeToPTY) —
    // pre-PtyMux these were also racing without synchronization
    // but it never showed because OSC writes are tiny + rare.
    std::mutex mWriteQueueMutex;
    std::vector<char> mWriteQueue;
    // Bytes read from the PTY and not yet handed to a parse task.
    // Written from the PtyMux thread's read callback (readFromFD);
    // drained by the parse worker in queueParse() under
    // mReadBufferMutex which serializes both ends. Distinct from
    // TerminalEmulator::mMutex (which serializes parse vs. snapshot)
    // so the mux thread can keep coalescing reads while the worker
    // parses.
    std::vector<char> mReadCoalesceBuffer;
    std::mutex mReadBufferMutex;
    // PTY read backpressure. When mReadCoalesceBuffer's size hits
    // kReadBufferHigh, readFromFD stops draining the kernel PTY
    // buffer and disarms POLLIN on the master fd. The kernel PTY
    // buffer then fills, and the child process blocks in write(2)
    // until it drains — exactly the same backpressure pattern kitty
    // and wezterm use to keep memory bounded under a flooding
    // producer (see kitty's vt-parser.c BUF_SZ + POLLIN-toggle and
    // wezterm's 1 MiB socketpair). Re-armed by the worker thread
    // (Terminal::queueParse loop) once the buffer drops below
    // kReadBufferLow. Atomic: set by the PtyMux thread (in
    // readFromFD) and CAS-cleared by the worker (in maybeResumeRead).
    std::atomic<bool> mReadPaused { false };
    // High/low watermarks trade input-latency vs throughput. Each
    // hit of the high-water disarms POLLIN, makes the producer
    // block in write(2), and pays a kqueue/epoll rearm round-trip
    // once the parser drains below low-water. For megabyte-per-
    // frame producers (e.g. mpv -vo=kitty with t=d, ~5.4 MiB per
    // frame at 1080p RGB24) a 64 KiB high-water meant ~85 rearm
    // round-trips per frame and capped throughput well below what
    // the parser could chew. 1 MiB cuts that to ~6 per frame.
    // Cost: at sustained flooding workloads the parser can hold
    // mMutex up to ~chunk_size / throughput; input/snapshot/resize
    // wait that long worst-case.
    static constexpr size_t kReadBufferHigh = 1024 * 1024; // 1 MiB
    static constexpr size_t kReadBufferLow  = 256 * 1024;  // 256 KiB

public:
    // Called by the parse worker after each batch of injectData
    // completes. If the read backpressure paused PTY reads, check
    // whether the coalesce buffer has drained below the low-water
    // mark and re-arm POLLIN on the PtyMux poller if so.
    void maybeResumeRead();

private:
    // Counts queued+running parse tasks for this Terminal. Always 0 or 1
    // in normal operation (queueParse is gated by this flag), but typed
    // as int so the graveyard can defer destruction while a worker still
    // references the Terminal.
    std::atomic<int> mParseInFlight { 0 };

    // Pixel rect in the window
    Rect mRect;

    // Cell-coordinate position within parent (for popup children)
    int mCellX { 0 }, mCellY { 0 }, mCellW { 0 }, mCellH { 0 };

    // String identifier for popup children (set by createPopup, used for lookup)
    std::string mPopupId;

    // Metadata set by OSC callbacks. Title/icon live on the emulator's
    // XTWINOPS stacks (pull-model via Terminal::title/icon).
    int mProgressState { 0 };
    int mProgressPct { 0 };
    std::string mCWD;
    // JS-set title override. When engaged, Terminal::title() returns this
    // instead of the OSC stack top. Set via pane.title = "..." and cleared
    // by pane.title = null/undefined.
    std::optional<std::string> mCustomTitle;

    // Popup children — headless child terminals at cell coordinates
    std::vector<std::unique_ptr<Terminal>> mPopups;
    std::string mFocusedPopupId;

    // Embedded children — headless terminals anchored to a logical line id,
    // rendered inline in the scrollback. Destroyed when the anchor row
    // evicts past the archive cap (via Document::onRowEvicted callback wired
    // in the constructor). Keyed on lineId so each row holds at most one.
    //
    // Protected by mMutex. The parse worker holds mMutex during the
    // apply phase of injectData (parse runs lock-free under
    // mParseStateMutex), and the eviction callback fires while the
    // worker holds mMutex. Main-thread mutators (createEmbedded,
    // extractEmbedded, resizeEmbedded) and readers (forEachEmbedded
    // etc.) take mMutex between apply phases.
    std::unordered_map<uint64_t, std::unique_ptr<Terminal>> mEmbedded;
    // Read on the per-tick render path; written by main-thread focus
    // mutators and the parse-worker eviction callback. Atomic to
    // avoid a lock just for this single uint64_t read.
    std::atomic<uint64_t> mFocusedEmbeddedLineId { 0 }; // 0 = none focused

    // ─────────────────────────────────────────────────────────────────
    // Output-capture internals (see public API + threading note above)

    // Text-format stripper state. Lives per-capture so escape
    // sequences split across chunk boundaries (very common: a CSI
    // can land as "\x1b[" then "31m" in two PTY reads) reassemble
    // correctly. State machine is a small subset of the VT/xterm
    // parser — we recognise sequence kinds well enough to know
    // when they end, but throw all of them away.
    enum class TextStripState
    {
        Ground,          // normal text — pass printable / UTF-8 / LF / TAB
        Esc,             // saw ESC (0x1b); next byte selects the kind
        Csi,             // ESC [ ... <terminator 0x40-0x7E>
        Osc,             // ESC ] ... <BEL or ESC \>
        OscEsc,          // saw ESC inside OSC — next byte may be ST (\\)
        Dcs,             // ESC P ... <ESC \>
        DcsEsc,          // saw ESC inside DCS — next byte may be ST
        Ss3,             // ESC O <single byte>
        EscIntermediate, // ESC followed by 0x20-0x2F (intermediate); next byte is final
        SeenCr,          // saw CR; if next is LF emit only LF, else
                         //   drop the CR and re-process this byte
    };

    struct CaptureEntry
    {
        std::string path;
        CaptureFormat format;
        // FILE* over int fd: we want buffered writes (single fwrite per
        // chunk + per-write fflush in asciicast mode for crash safety
        // of the latest record). std::ofstream would do, but FILE* lets
        // us reach for fileno()+fsync() later without re-plumbing.
        FILE *fp { nullptr };
        // Per-capture mutex serialises writes from main + worker.
        // Held only across a single record write (fwrite + maybe
        // fflush), so contention windows are short.
        std::mutex mu;
        // Once stopped, producers must not write. Set under `mu`.
        // Producers re-check after acquiring `mu` so a concurrent stop
        // is observed before the write.
        bool stopped { false };
        // Asciicast v2 needs a wall-clock origin for relative
        // timestamps. Captured at registration time. Steady-clock would
        // give monotonic time but asciicast records *real* timestamps
        // (replay tools display elapsed since record start), so steady
        // is fine — the absolute value is meaningless to the format.
        std::chrono::steady_clock::time_point start;
        // Text-format parser state. Untouched for Raw / Asciicast.
        TextStripState textState { TextStripState::Ground };
    };

    // Wrap each entry in a unique_ptr so the per-entry mutex has a
    // stable address even when the vector resizes. shared_ptr would
    // also work; unique_ptr suffices because the vector owns and
    // producers only ever borrow.
    std::vector<std::unique_ptr<CaptureEntry>> mCaptures; // protected by mCapturesMutex
    mutable std::mutex mCapturesMutex;

protected:
    // Producer-side tap. Snapshots the captures vector under the
    // mutex (cheap pointer copy), then iterates without holding the
    // outer lock; each CaptureEntry serialises its own writes via
    // its own mutex. Called from flushReadBuffer (main) and the
    // queueParse worker (off-thread). Failures inside any single
    // capture stop only that capture and fire onCaptureStopped — the
    // others continue normally. Protected (not private) so test
    // subclasses can drive the tap synchronously without spinning up
    // a real PTY.
    void deliverCapturedOutput(const char *data, size_t len);

private:
    // Stop a capture and close its file. Caller owns whether to
    // notify (Explicit stops do; IoError stops also do, with a
    // populated errorMessage). MUST be called WITHOUT holding
    // entry->mu — closeOne grabs it. errorOut is the message that
    // will travel to onCaptureStopped on IoError.
    void stopCaptureLocked(CaptureEntry *entry,
                           CaptureStopReason reason,
                           const std::string &errorMessage);

    // Embeddeds that the eviction callback extracted from mEmbedded.
    // The callback fires while the parse worker holds mMutex (deep
    // inside Document::evictToArchive); we stash the unique_ptr here
    // and let the main-thread Platform drain this list after the
    // parse batch completes, at which point it can take mMutex
    // briefly without contending. mEvictedHasItems is a wait-free
    // poll so the per-tick check never has to lock.
    struct EvictedEmbedded
    {
        uint64_t lineId;
        std::unique_ptr<Terminal> term;
    };

    std::vector<EvictedEmbedded> mEvictedEmbeddeds; // protected by mMutex
    std::atomic<bool> mEvictedHasItems { false };

public:
    // Called by Platform on each main-thread tick.  Moves
    // `fn(lineId, unique_ptr)` so Platform can graveyard-defer and
    // mirror into the render shadow copy. Takes mMutex.
    template <typename Fn>
    void drainEvictedEmbeddeds(Fn &&fn)
    {
        std::vector<EvictedEmbedded> drained;
        {
            std::lock_guard<std::recursive_mutex> _lk(mutex());
            drained.swap(mEvictedEmbeddeds);
            mEvictedHasItems.store(false, std::memory_order_release);
        }
        for (auto &e : drained) {
            fn(e.lineId, std::move(e.term));
        }
    }

    // True if there are evicted embeddeds waiting for the main thread
    // to graveyard them. Wait-free — main-thread tick polls this every
    // iteration, so it must never block on the parse worker's mMutex.
    bool hasEvictedEmbeddeds() const
    {
        return mEvictedHasItems.load(std::memory_order_acquire);
    }

private:
    // Disarm the PTY fd in the event loop and fire onTerminalExited (once).
    // Safe to call multiple times — the first call sets mExited.
    void markExited();

    // Set up / tear down the shell-pid exit watch. See mShellPid declaration
    // for the rationale. Idempotent.
    void startShellPidWatch();
    void stopShellPidWatch();
};
