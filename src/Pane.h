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
    std::unique_ptr<TerminalEmulator> emulator;
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

    // Popup panes (OSC 999 driven, no PTY)
    PopupPane* findPopup(const std::string& id);
    const std::vector<PopupPane>& popups() const { return mPopups; }
    std::string focusedPopupId() const { return mFocusedPopupId; }
    PopupPane* focusedPopup();

    // Rect (pixel coords, set by Layout::computeRects)
    void setRect(const PaneRect& r) { mRect = r; }
    const PaneRect& rect() const { return mRect; }

    // Resize the contained terminal to fit current rect given font metrics
    void resizeToRect(float charW, float lineH);

    // Set by PlatformDawn; called when a popup emulator fires an event (triggers redraw)
    std::function<void()> onPopupEvent;

private:
    void installOSCCallback(Terminal* t);
    void handleOSCMB(std::string_view payload);

    PopupPane* createPopup(const std::string& id, int x, int y, int w, int h);
    void destroyPopup(const std::string& id);
    void focusPopup(const std::string& id);
    void blurPopup(const std::string& id);

    int mId;
    std::unique_ptr<Terminal> terminal_;
    std::vector<PopupPane> mPopups;
    std::string mFocusedPopupId;
    PaneRect mRect;
};
