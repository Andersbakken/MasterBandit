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

void Pane::resizeToRect(float charW, float lineH)
{
    if (mRect.isEmpty()) return;
    int cols = std::max(1, static_cast<int>(mRect.w / charW));
    int rows = std::max(1, static_cast<int>(mRect.h / lineH));
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
    t->setOSCMBCallback([this](std::string_view payload) {
        handleOSCMB(payload);
    });
}

static std::unordered_map<std::string, std::string> parseKV(std::string_view s)
{
    std::unordered_map<std::string, std::string> kv;
    while (!s.empty()) {
        auto semi = s.find(';');
        std::string_view token = (semi == std::string_view::npos) ? s : s.substr(0, semi);
        auto eq = token.find('=');
        if (eq != std::string_view::npos)
            kv[std::string(token.substr(0, eq))] = std::string(token.substr(eq + 1));
        if (semi == std::string_view::npos) break;
        s = s.substr(semi + 1);
    }
    return kv;
}

void Pane::handleOSCMB(std::string_view payload)
{
    auto semi = payload.find(';');
    if (semi == std::string_view::npos) return;
    std::string_view verb = payload.substr(0, semi);
    std::string_view rest = payload.substr(semi + 1);

    if (verb == "create") {
        auto kv = parseKV(rest);
        auto toInt = [&kv](const char* key, int def = 0) {
            auto it = kv.find(key);
            if (it == kv.end()) return def;
            int v = def;
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
            return v;
        };
        auto idIt = kv.find("id");
        if (idIt == kv.end()) { spdlog::warn("Pane: OSC MB create missing id"); return; }
        createPopup(idIt->second, toInt("x"), toInt("y"), toInt("w", 1), toInt("h", 1));

    } else if (verb == "write") {
        // rest = "id=<id>;<content>"
        auto idSemi = rest.find(';');
        if (idSemi == std::string_view::npos) return;
        std::string_view idField = rest.substr(0, idSemi);
        std::string_view content = rest.substr(idSemi + 1);
        if (idField.size() < 3 || idField.substr(0, 3) != "id=") return;
        std::string id(idField.substr(3));
        PopupPane* popup = findPopup(id);
        if (!popup) { spdlog::warn("Pane: OSC MB write to unknown popup '{}'", id); return; }
        popup->emulator->injectData(content.data(), content.size());

    } else if (verb == "destroy") {
        auto kv = parseKV(rest);
        auto idIt = kv.find("id");
        if (idIt != kv.end()) destroyPopup(idIt->second);

    } else if (verb == "focus") {
        auto kv = parseKV(rest);
        auto idIt = kv.find("id");
        if (idIt != kv.end()) focusPopup(idIt->second);

    } else if (verb == "blur") {
        auto kv = parseKV(rest);
        auto idIt = kv.find("id");
        if (idIt != kv.end()) blurPopup(idIt->second);

    } else {
        spdlog::warn("Pane: unknown OSC MB verb '{}'", verb);
    }
}

PopupPane* Pane::createPopup(const std::string& id, int x, int y, int w, int h)
{
    if (findPopup(id)) {
        spdlog::warn("Pane: popup '{}' already exists", id);
        return findPopup(id);
    }

    PopupPane popup;
    popup.id = id;
    popup.cellX = x;
    popup.cellY = y;
    popup.cellW = w;
    popup.cellH = h;

    TerminalCallbacks cbs;
    cbs.event = [this](TerminalEmulator*, int, void*) {
        if (onPopupEvent) onPopupEvent();
    };
    popup.emulator = std::make_unique<TerminalEmulator>(std::move(cbs));
    popup.emulator->resize(w, h);

    spdlog::info("Pane: created popup '{}' at ({},{}) {}x{}", id, x, y, w, h);
    mPopups.push_back(std::move(popup));
    return &mPopups.back();
}

void Pane::destroyPopup(const std::string& id)
{
    if (mFocusedPopupId == id) mFocusedPopupId.clear();
    auto it = std::find_if(mPopups.begin(), mPopups.end(),
                           [&id](const PopupPane& p) { return p.id == id; });
    if (it == mPopups.end()) {
        spdlog::warn("Pane: destroy unknown popup '{}'", id);
        return;
    }
    spdlog::info("Pane: destroyed popup '{}'", id);
    mPopups.erase(it);
}

void Pane::focusPopup(const std::string& id)
{
    if (!findPopup(id)) { spdlog::warn("Pane: focus unknown popup '{}'", id); return; }
    mFocusedPopupId = id;
    spdlog::info("Pane: focused popup '{}'", id);
}

void Pane::blurPopup(const std::string& id)
{
    if (mFocusedPopupId == id) mFocusedPopupId.clear();
}
