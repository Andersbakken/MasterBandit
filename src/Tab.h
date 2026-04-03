#pragma once

#include "Layout.h"
#include "Terminal.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Tab {
public:
    explicit Tab(std::unique_ptr<Layout> layout);

    Layout* layout() { return layout_.get(); }
    const Layout* layout() const { return layout_.get(); }

    const std::string& title() const { return title_; }
    void setTitle(const std::string& t) { title_ = t; }

    // Icon (from OSC 1) and progress (from OSC 9;4)
    const std::string& icon() const { return icon_; }
    void setIcon(const std::string& s) { icon_ = s; }
    int progressState() const { return progressState_; }
    int progressPct() const { return progressPct_; }
    void setProgress(int state, int pct) { progressState_ = state; progressPct_ = pct; }

    // Full-screen overlay (kitty-style): covers the entire tab's layout area.
    // Not shown in the tab bar.
    void pushOverlay(std::unique_ptr<Terminal> t);
    void popOverlay();
    bool hasOverlay() const { return !overlays_.empty(); }
    Terminal* topOverlay();

    // The active visible content: top overlay if any, else nullptr (render layout normally).
    Terminal* activeOverlay();

    // Callback set by PlatformDawn when an overlay is popped (to close its PTY poll).
    std::function<void(int /*masterFD*/)> onOverlayPopped;

private:
    std::unique_ptr<Layout> layout_;
    std::string title_;
    std::string icon_;
    int progressState_ = 0;   // 0=none 1=set 2=error 3=indeterminate 4=paused
    int progressPct_   = 0;   // 0-100, used when state==1 or 2
    std::vector<std::unique_ptr<Terminal>> overlays_;
};
