#pragma once

#include "Terminal.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct PaneRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool isEmpty() const { return w == 0 || h == 0; }
};

struct PopupPane {
    std::string id;
    int cellX = 0, cellY = 0, cellW = 0, cellH = 0;
    std::unique_ptr<Terminal> terminal;  // headless Terminal for script-driven popups
};

class Pane {
public:
    explicit Pane(int id);

    int id() const { return mId; }

    // Terminal (one per pane)
    void setTerminal(std::unique_ptr<Terminal> t);
    Terminal* terminal() { return terminal_.get(); }

    // Active terminal: just returns terminal_.get()
    TerminalEmulator* activeTerm();

    // Popup panes — script-driven floating cell grids
    PopupPane* createPopup(const std::string& id, int x, int y, int w, int h,
                           PlatformCallbacks pcbs);
    void destroyPopup(const std::string& id);
    bool resizePopup(const std::string& id, int x, int y, int w, int h);
    PopupPane* findPopup(const std::string& id);
    const std::vector<PopupPane>& popups() const { return mPopups; }
    std::string focusedPopupId() const { return mFocusedPopupId; }
    void setFocusedPopup(const std::string& id) { mFocusedPopupId = id; }
    void clearFocusedPopup() { mFocusedPopupId.clear(); }
    PopupPane* focusedPopup();

    // Title and icon set by the shell via OSC sequences
    const std::string& title() const { return mTitle; }
    void setTitle(const std::string& t) { mTitle = t; }
    const std::string& icon() const { return mIcon; }
    void setIcon(const std::string& i) { mIcon = i; }

    int progressState() const { return mProgressState; }
    int progressPct() const { return mProgressPct; }
    void setProgress(int state, int pct) { mProgressState = state; mProgressPct = pct; }

    const std::string& cwd() const { return mCWD; }
    void setCWD(const std::string& d) { mCWD = d; }

    // Rect (pixel coords, set by Layout::computeRects)
    void setRect(const PaneRect& r) { mRect = r; }
    const PaneRect& rect() const { return mRect; }

    // Resize the contained terminal to fit current rect given font metrics
    void resizeToRect(float charW, float lineH, float padL = 0, float padT = 0, float padR = 0, float padB = 0);

    // Set by PlatformDawn; called when a popup emulator fires an event (triggers redraw)
    std::function<void()> onPopupEvent;

private:
    void installOSCCallback(Terminal* t);

    int mId;
    std::unique_ptr<Terminal> terminal_;
    std::vector<PopupPane> mPopups;
    std::string mFocusedPopupId;
    PaneRect mRect;
    std::string mTitle;
    std::string mIcon;
    int mProgressState = 0;
    int mProgressPct = 0;
    std::string mCWD;
};
