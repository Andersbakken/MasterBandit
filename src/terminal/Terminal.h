#pragma once
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
#include <eventloop/EventLoop.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

// Pixel rectangle in the window coordinate space.
struct PaneRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool isEmpty() const { return w == 0 || h == 0; }
};

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
    int id() const { return mId; }
    void setId(int id) { mId = id; }

    // --- PTY lifecycle ---
    bool init(const TerminalOptions& options);
    // Initialize without a PTY or child process. For script-driven applets.
    bool initHeadless(const TerminalOptions& options);
    void setLoop(EventLoop* loop) { mLoop = loop; }
    int masterFD() const { return mMasterFD; }
    bool isHeadless() const { return mHeadless; }
    void readFromFD();
    void flushReadBuffer();
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

    // --- Pixel rect (set by Layout::computeRects for panes) ---
    void setRect(const PaneRect& r) { mRect = r; }
    const PaneRect& rect() const { return mRect; }

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

    // --- Title, icon, progress, CWD (set by OSC callbacks) ---
    const std::string& title() const { return mTitle; }
    void setTitle(const std::string& t) { mTitle = t; }
    const std::string& icon() const { return mIcon; }
    void setIcon(const std::string& i) { mIcon = i; }
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

    // Returns the focused popup's emulator if one exists, otherwise this.
    TerminalEmulator* activeTerm();

    // Set by platform; called when a popup emulator fires an event (triggers redraw)
    std::function<void()> onPopupEvent;

protected:
    void writeToOutput(const char* data, size_t len) override;

private:
    void writeToPTY(const char* data, size_t len);
    PlatformCallbacks mPlatformCbs;
    TerminalOptions mOptions;
    int mId { -1 };
    int mMasterFD { -1 };
    bool mHeadless { false };
    std::atomic<bool> mResizePending { false };
    pid_t mLastFgPgid { -1 };
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
    std::vector<char> mReadCoalesceBuffer;

    // Pixel rect in the window
    PaneRect mRect;

    // Cell-coordinate position within parent (for popup children)
    int mCellX { 0 }, mCellY { 0 }, mCellW { 0 }, mCellH { 0 };

    // String identifier for popup children (set by createPopup, used for lookup)
    std::string mPopupId;

    // Metadata set by OSC callbacks
    std::string mTitle;
    std::string mIcon;
    int mProgressState { 0 };
    int mProgressPct { 0 };
    std::string mCWD;

    // Popup children — headless child terminals at cell coordinates
    std::vector<std::unique_ptr<Terminal>> mPopups;
    std::string mFocusedPopupId;

    // Disarm the PTY fd in the event loop and fire onTerminalExited (once).
    // Safe to call multiple times — the first call sets mExited.
    void markExited();
};
