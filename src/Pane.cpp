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
    if (auto* pp = focusedPopup())
        return pp->terminal.get();
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
    // set in buildTerminalCallbacks.
}

PopupPane* Pane::createPopup(const std::string& id, int x, int y, int w, int h,
                              PlatformCallbacks pcbs)
{
    if (findPopup(id)) {
        spdlog::warn("Pane: popup '{}' already exists", id);
        return nullptr;
    }

    TerminalCallbacks cbs;
    cbs.event = [this](TerminalEmulator*, int, void*) {
        if (onPopupEvent) onPopupEvent();
    };

    auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    TerminalOptions opts;
    opts.scrollbackLines = 0;
    terminal->initHeadless(opts);
    terminal->resize(w, h);

    PopupPane popup;
    popup.id = id;
    popup.cellX = x;
    popup.cellY = y;
    popup.cellW = w;
    popup.cellH = h;
    popup.terminal = std::move(terminal);

    spdlog::info("Pane: created popup '{}' at ({},{}) {}x{}", id, x, y, w, h);
    mPopups.push_back(std::move(popup));
    return &mPopups.back();
}

bool Pane::resizePopup(const std::string& id, int x, int y, int w, int h)
{
    auto* p = findPopup(id);
    if (!p) return false;
    p->cellX = x;
    p->cellY = y;
    p->cellW = w;
    p->cellH = h;
    if (p->terminal) p->terminal->resize(w, h);
    return true;
}

std::optional<PopupPane> Pane::extractPopup(const std::string& id)
{
    auto it = std::find_if(mPopups.begin(), mPopups.end(),
                           [&id](const PopupPane& p) { return p.id == id; });
    if (it == mPopups.end()) return std::nullopt;
    PopupPane popup = std::move(*it);
    mPopups.erase(it);
    if (mFocusedPopupId == id) mFocusedPopupId.clear();
    spdlog::info("Pane: extracted popup '{}'", id);
    return popup;
}

