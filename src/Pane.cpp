#include "Pane.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <charconv>
#include <unordered_map>

Pane::Pane(int id)
    : mId(id)
{
}

void Pane::setTerminal(std::unique_ptr<Terminal> t)
{
    terminal_ = std::move(t);
    if (terminal_) installOSCCallback(terminal_.get());
}

TerminalEmulator* Pane::activeTerm()
{
    return terminal_.get();
}

void Pane::resizeToRect(float charW, float lineH, float padL, float padT, float padR, float padB)
{
    if (mRect.isEmpty()) return;
    float usableW = std::max(0.0f, static_cast<float>(mRect.w) - padL - padR);
    float usableH = std::max(0.0f, static_cast<float>(mRect.h) - padT - padB);
    int cols = std::max(1, static_cast<int>(usableW / charW));
    int rows = std::max(1, static_cast<int>(usableH / lineH));
    if (terminal_) terminal_->resize(cols, rows);
}

PopupPane* Pane::findPopup(const std::string& id)
{
    for (auto& p : mPopups)
        if (p.id == id) return &p;
    return nullptr;
}

PopupPane* Pane::focusedPopup()
{
    if (mFocusedPopupId.empty()) return nullptr;
    return findPopup(mFocusedPopupId);
}

void Pane::installOSCCallback(Terminal* t)
{
    // No-op: OSC handling is done via the script engine's onOSC callback
    // set in buildTerminalCallbacks. Popup management will move to JS.
}

