#pragma once
#include "Rect.h"
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
#include "Uuid.h"
#include <eventloop/EventLoop.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Terminal; // forward declare for PlatformCallbacks

struct PlatformCallbacks {
    std::function<void(Terminal*)> onTerminalExited;
    std::function<void()> quit;
    // Output filter: check shouldFilterOutput first (cheap, no copy),
    // then call filterOutput only if it returns true.
    std::function<bool()> shouldFilterOutput;
    std::function<void(std::string& data)> filterOutput;
    // Input filter: same pattern.
    std::function<bool()> shouldFilterInput;
    std::function<void(std::string& data)> filterInput;
    // Headless input sink: receives keystrokes when no PTY exists (applet mode).
    std::function<void(const char* data, size_t len)> onInput;
};

class Terminal : public TerminalEmulator {
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
    bool init(const TerminalOptions& options);
    // Initialize without a PTY or child process. For script-driven applets.
    bool initHeadless(const TerminalOptions& options);
    void setLoop(EventLoop* loop) { mLoop = loop; }
    int masterFD() const { return mMasterFD; }
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
    bool queueParse(const ParseSubmitFn& submit);

    // True iff a parse task is queued or currently running on a worker.
    // Used by the graveyard to defer destruction until parsing finishes.
    bool parseInFlight() const {
        return mParseInFlight.load(std::memory_order_acquire) != 0;
    }
    void flushWriteQueue();
    // Paste: wraps in \x1b[200~/\x1b[201~ when DECSET 2004 is active on the
    // terminal. Use for real clipboard/selection pastes so the shell's paste
    // handling (quoting, auto-suggest suppression, etc.) takes effect.
    void pasteText(const std::string& text);
    // Write: raw send to the PTY, no bracketing. Use for synthetic keystrokes,
    // OSC responses, or anything that isn't semantically a user paste.
    void writeText(const std::string& text) { writeToPTY(text.data(), text.size()); }

    // Query the foreground process name via tcgetpgrp + platform process lookup
    std::string foregroundProcess() const;
    void resize(int width, int height) override;

    // Deferred TIOCSWINSZ: set by resize(), consumed by flushPendingResize()
    bool hasResizePending() const { return mResizePending.load(std::memory_order_acquire); }
    void flushPendingResize();

    // --- Pixel rect (set by Engine::computeTabRects for panes) ---
    void setRect(const Rect& r) { mRect = r; }
    const Rect& rect() const { return mRect; }

    // Resize the terminal to fit current rect given font metrics
    void resizeToRect(float charW, float lineH,
                      float padL = 0, float padT = 0,
                      float padR = 0, float padB = 0);

    // --- Cell-coordinate position (for popup children / embedded terminals) ---
    int cellX() const { return mCellX; }
    int cellY() const { return mCellY; }
    int cellW() const { return mCellW; }
    int cellH() const { return mCellH; }
    void setCellPosition(int x, int y, int w, int h) {
        mCellX = x; mCellY = y; mCellW = w; mCellH = h;
    }

    // --- Popup string identifier (set on popup children) ---
    const std::string& popupId() const { return mPopupId; }
    void setPopupId(const std::string& id) { mPopupId = id; }

    // --- Title, icon, progress, CWD ---
    // title/icon are pull-model: reads the emulator's XTWINOPS stack.
    // nullopt means "no OSC-set title" — callers decide the fallback.
    std::optional<std::string> title() const { return currentTitle(); }
    std::optional<std::string> icon() const  { return currentIcon(); }
    int progressState() const { return mProgressState; }
    int progressPct() const { return mProgressPct; }
    void setProgress(int state, int pct) { mProgressState = state; mProgressPct = pct; }
    const std::string& cwd() const { return mCWD; }
    void setCWD(const std::string& d) { mCWD = d; }

    // --- Popup children ---
    // Create a headless child terminal at cell coordinates.
    Terminal* createPopup(const std::string& id, int x, int y, int w, int h,
                          PlatformCallbacks pcbs);
    // Move a popup out for deferred destruction. Returns the moved Terminal
    // on success, or nullptr if not found.
    std::unique_ptr<Terminal> extractPopup(const std::string& id);
    bool resizePopup(const std::string& id, int x, int y, int w, int h);
    Terminal* findPopup(const std::string& id);
    const std::vector<std::unique_ptr<Terminal>>& popups() const { return mPopups; }

    // True if (col, row) lies inside any popup child's cell rect.
    bool isCellCoveredByPopup(int col, int row) const;

    std::string focusedPopupId() const { return mFocusedPopupId; }
    void setFocusedPopup(const std::string& id) { mFocusedPopupId = id; }
    void clearFocusedPopup() { mFocusedPopupId.clear(); }
    Terminal* focusedPopup();

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
    Terminal* createEmbedded(int rows, PlatformCallbacks pcbs);
    // Move an embedded out for deferred destruction. Returns the moved
    // Terminal on success, or nullptr if no embedded is anchored at lineId.
    std::unique_ptr<Terminal> extractEmbedded(uint64_t lineId);
    // Resize an existing embedded. Only rows change; cols track parent cols.
    bool resizeEmbedded(uint64_t lineId, int rows);
    Terminal* findEmbedded(uint64_t lineId);

    // Embedded-map iteration helpers. All acquire mMutex (which the
    // parse worker also holds during its apply phase). Use these
    // instead of exposing the map directly so callers can't forget
    // the lock.
    template <typename Fn>
    void forEachEmbedded(Fn&& fn) const {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        for (const auto& [lineId, em] : mEmbedded) fn(lineId, *em);
    }
    bool hasEmbeddeds() const {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        return !mEmbedded.empty();
    }
    size_t embeddedCount() const {
        std::lock_guard<std::recursive_mutex> _lk(mutex());
        return mEmbedded.size();
    }

    // Atomic: written by the parse-worker eviction callback (clears to 0
    // when the focused embedded's anchor evicts) and by main-thread
    // create/destroy/focus paths. Read on the per-tick render path.
    uint64_t focusedEmbeddedLineId() const { return mFocusedEmbeddedLineId.load(std::memory_order_acquire); }
    void setFocusedEmbeddedLineId(uint64_t id) { mFocusedEmbeddedLineId.store(id, std::memory_order_release); }
    void clearFocusedEmbedded() { mFocusedEmbeddedLineId.store(0, std::memory_order_release); }
    Terminal* focusedEmbedded();

    // Returns the focused popup's emulator if one exists, else the focused
    // embedded's emulator if any, else this.
    TerminalEmulator* activeTerm();

    // Hook consumed by TerminalSnapshot::update() to build the visual-layout
    // segment list. Pushes (lineId, rowCount) for every embedded currently
    // attached to this Terminal. Alt-screen hides embeddeds so the list
    // stays empty there.
    void collectEmbeddedAnchors(std::vector<EmbeddedAnchor>& out) const override;

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
                            uint64_t& outLineId,
                            int& outRelCol, int& outRelRow,
                            int& outRelPixelX, int& outRelPixelY) const;

    // Set by platform; called when a popup or embedded emulator fires an event (triggers redraw)
    std::function<void()> onPopupEvent;

    // Set by platform; called when an embedded gets resized as a side
    // effect of the parent pane's cols changing. Fires after the resize
    // so the callback can read `em->width()/height()` directly. Platform
    // wires this to scriptEngine_.deliverEmbeddedResized so the JS
    // "resized" event fires uniformly whether the resize was initiated
    // by JS (em.resize) or cascaded from the parent.
    std::function<void(uint64_t lineId, int cols, int rows)> onEmbeddedResized;

protected:
    void writeToOutput(const char* data, size_t len) override;

private:
    void writeToPTY(const char* data, size_t len);
    PlatformCallbacks mPlatformCbs;
    TerminalOptions mOptions;
    Uuid mNodeId;
    int mMasterFD { -1 };
    bool mHeadless { false };
    std::atomic<bool> mResizePending { false };
    // Read/written from both the main thread (flushReadBuffer fallback)
    // and the parse worker (queueParse loop) in mutually exclusive
    // code paths today, but typed as atomic so the absence of a
    // formal happens-before is no longer UB and so future readers
    // (e.g. for stats) can sample without locks.
    std::atomic<pid_t> mLastFgPgid { -1 };
    EventLoop* mLoop { nullptr };
    EventLoop::TimerId mWritePollId { 0 };
    bool mWritePollActive { false };
    // Set once the PTY has delivered EOF or an unrecoverable I/O error.
    // Further read/write attempts short-circuit, and the fd is disarmed
    // from the event loop so kqueue/epoll don't re-fire on the persistent
    // EV_EOF and busy-spin the main thread before onTick can drain the
    // deferred terminalExited callback.
    bool mExited { false };
    std::vector<char> mWriteQueue;
    // Bytes read from the PTY and not yet handed to a parse task. Written
    // from the main-thread fd callback (readFromFD); drained from main in
    // queueParse() under mReadBufferMutex which is also taken by the
    // worker when it loops to pick up newly-arrived data. Distinct from
    // TerminalEmulator::mMutex (which serializes parse vs. snapshot) so
    // the main thread can keep coalescing reads while the worker parses.
    std::vector<char> mReadCoalesceBuffer;
    std::mutex        mReadBufferMutex;
    // PTY read backpressure. When mReadCoalesceBuffer's size hits
    // kReadBufferHigh, readFromFD stops draining the kernel PTY
    // buffer and disarms POLLIN on the master fd. The kernel PTY
    // buffer then fills, and the child process blocks in write(2)
    // until it drains — exactly the same backpressure pattern kitty
    // and wezterm use to keep memory bounded under a flooding
    // producer (see kitty's vt-parser.c BUF_SZ + POLLIN-toggle and
    // wezterm's 1 MiB socketpair). Re-armed by the main-thread
    // onTick when the buffer drops below kReadBufferLow.
    bool              mReadPaused { false };
    // High/low watermarks tuned so a single parser apply holds
    // mMutex for at most ~50 ms even at the worst observed parser
    // throughput (~1 MB/s under sustained scrollback growth). Other
    // threads wanting mMutex (input handlers, render snapshot,
    // resize) will see at most that wait. Smaller values reduce
    // worst-case latency further but pay more PTY-poll round-trips
    // and slightly hurt throughput on bursty workloads.
    static constexpr size_t kReadBufferHigh = 64 * 1024;   // 64 KiB
    static constexpr size_t kReadBufferLow  = 16 * 1024;   // 16 KiB
public:
    // Called by Platform on each main-thread tick. If the read
    // backpressure paused PTY reads, check whether the coalesce
    // buffer has drained below the low-water mark and re-arm POLLIN
    // if so.
    void maybeResumeRead();
private:
    // Counts queued+running parse tasks for this Terminal. Always 0 or 1
    // in normal operation (queueParse is gated by this flag), but typed
    // as int so the graveyard can defer destruction while a worker still
    // references the Terminal.
    std::atomic<int>  mParseInFlight { 0 };

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

    // Embeddeds that the eviction callback extracted from mEmbedded.
    // The callback fires while the parse worker holds mMutex (deep
    // inside Document::evictToArchive); we stash the unique_ptr here
    // and let the main-thread Platform drain this list after the
    // parse batch completes, at which point it can take mMutex
    // briefly without contending. mEvictedHasItems is a wait-free
    // poll so the per-tick check never has to lock.
    struct EvictedEmbedded { uint64_t lineId; std::unique_ptr<Terminal> term; };
    std::vector<EvictedEmbedded> mEvictedEmbeddeds; // protected by mMutex
    std::atomic<bool>            mEvictedHasItems { false };

public:
    // Called by Platform on each main-thread tick.  Moves
    // `fn(lineId, unique_ptr)` so Platform can graveyard-defer and
    // mirror into the render shadow copy. Takes mMutex.
    template <typename Fn>
    void drainEvictedEmbeddeds(Fn&& fn) {
        std::vector<EvictedEmbedded> drained;
        {
            std::lock_guard<std::recursive_mutex> _lk(mutex());
            drained.swap(mEvictedEmbeddeds);
            mEvictedHasItems.store(false, std::memory_order_release);
        }
        for (auto& e : drained) fn(e.lineId, std::move(e.term));
    }
    // True if there are evicted embeddeds waiting for the main thread
    // to graveyard them. Wait-free — main-thread tick polls this every
    // iteration, so it must never block on the parse worker's mMutex.
    bool hasEvictedEmbeddeds() const {
        return mEvictedHasItems.load(std::memory_order_acquire);
    }

private:

    // Disarm the PTY fd in the event loop and fire onTerminalExited (once).
    // Safe to call multiple times — the first call sets mExited.
    void markExited();
};
