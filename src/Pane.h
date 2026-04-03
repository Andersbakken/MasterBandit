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

    // Tabs (shown in tab bar)
    void addTab(std::unique_ptr<Terminal> terminal);
    Terminal* activeTab();
    const std::vector<std::unique_ptr<Terminal>>& tabs() const { return mTabs; }
    void setActiveTab(int idx);
    int activeTabIndex() const { return mActiveTab; }

    // Overlays (full-screen, not in tab bar, rendered on top of active tab)
    void pushOverlay(std::unique_ptr<Terminal> terminal);
    void popOverlay();
    bool hasOverlay() const { return !mOverlays.empty(); }
    Terminal* topOverlay();

    // Active terminal: top overlay if any, else active tab
    TerminalEmulator* activeTerm();

    // Popup panes (OSC 999 driven, no PTY)
    PopupPane* findPopup(const std::string& id);
    const std::vector<PopupPane>& popups() const { return mPopups; }
    std::string focusedPopupId() const { return mFocusedPopupId; }
    PopupPane* focusedPopup();

    // Rect (pixel coords, set by Layout::computeRects)
    void setRect(const PaneRect& r) { mRect = r; }
    const PaneRect& rect() const { return mRect; }

    // Resize all contained terminals to fit current rect given font metrics
    void resizeToRect(float charW, float lineH);

    // Set by PlatformDawn; called with the masterFD when an overlay is popped
    std::function<void(int /*masterFD*/)> onOverlayPopped;

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
    std::vector<std::unique_ptr<Terminal>> mTabs;
    int mActiveTab { 0 };
    std::vector<std::unique_ptr<Terminal>> mOverlays;
    std::vector<PopupPane> mPopups;
    std::string mFocusedPopupId;
    PaneRect mRect;
};
