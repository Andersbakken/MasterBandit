#include "Bindings.h"
#include "Config.h"
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_map>

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::optional<KeyStroke> parseKeyStroke(const std::string& s)
{
    // Split on '+', last token is the key name, earlier tokens are modifiers.
    std::vector<std::string> parts;
    std::string token;
    for (char c : s) {
        if (c == '+') {
            if (!token.empty()) { parts.push_back(token); token.clear(); }
        } else {
            token += c;
        }
    }
    if (!token.empty()) parts.push_back(token);
    if (parts.empty()) return std::nullopt;

    std::string keyStr = parts.back();
    parts.pop_back();

    unsigned int mods = 0;
    for (const auto& mod : parts) {
        std::string m = toLower(mod);
        if      (m == "ctrl" || m == "control")                       mods |= CtrlModifier;
        else if (m == "shift")                                         mods |= ShiftModifier;
        else if (m == "alt")                                           mods |= AltModifier;
        else if (m == "meta" || m == "super" || m == "cmd" || m == "win")
                                                                       mods |= MetaModifier;
        else {
            spdlog::warn("Bindings: unknown modifier '{}' in '{}'", mod, s);
            return std::nullopt;
        }
    }

    std::string ks = toLower(keyStr);
    Key key;

    if (ks.size() == 1 && ks[0] >= 'a' && ks[0] <= 'z') {
        key = static_cast<Key>(Key_A + (ks[0] - 'a'));
    } else if (ks.size() == 1 && ks[0] >= '0' && ks[0] <= '9') {
        key = static_cast<Key>(Key_0 + (ks[0] - '0'));
    } else if (ks == "[") {
        key = Key_BracketLeft;
    } else if (ks == "]") {
        key = Key_BracketRight;
    } else if (ks == "-" || ks == "minus") {
        key = Key_Minus;
    } else if (ks == "=" || ks == "equals") {
        key = Key_Equal;
    } else {
        static const std::unordered_map<std::string, Key> keyMap = {
            {"escape",    Key_Escape},
            {"tab",       Key_Tab},
            {"backspace", Key_Backspace},
            {"return",    Key_Return},
            {"enter",     Key_Return},
            {"space",     Key_Space},
            {"left",      Key_Left},
            {"right",     Key_Right},
            {"up",        Key_Up},
            {"down",      Key_Down},
            {"home",      Key_Home},
            {"end",       Key_End},
            {"pageup",    Key_PageUp},
            {"pagedown",  Key_PageDown},
            {"insert",    Key_Insert},
            {"delete",    Key_Delete},
            {"f1",  Key_F1},  {"f2",  Key_F2},  {"f3",  Key_F3},  {"f4",  Key_F4},
            {"f5",  Key_F5},  {"f6",  Key_F6},  {"f7",  Key_F7},  {"f8",  Key_F8},
            {"f9",  Key_F9},  {"f10", Key_F10}, {"f11", Key_F11}, {"f12", Key_F12},
        };
        auto it = keyMap.find(ks);
        if (it == keyMap.end()) {
            spdlog::warn("Bindings: unknown key '{}' in '{}'", keyStr, s);
            return std::nullopt;
        }
        key = it->second;
    }

    return KeyStroke{key, mods};
}

std::optional<Action::Any> parseAction(const std::string& name,
                                        const std::vector<std::string>& args)
{
    if (name == "new_tab")   return Action::NewTab{};
    if (name == "close_tab") return Action::CloseTab{};

    if (name == "activate_tab_relative") {
        if (args.empty()) {
            spdlog::warn("Bindings: activate_tab_relative requires an arg");
            return std::nullopt;
        }
        try { return Action::ActivateTabRelative{std::stoi(args[0])}; }
        catch (...) { spdlog::warn("Bindings: bad arg for activate_tab_relative"); return std::nullopt; }
    }
    if (name == "activate_tab") {
        if (args.empty()) {
            spdlog::warn("Bindings: activate_tab requires an arg");
            return std::nullopt;
        }
        try { return Action::ActivateTab{std::stoi(args[0])}; }
        catch (...) { spdlog::warn("Bindings: bad arg for activate_tab"); return std::nullopt; }
    }
    if (name == "split_pane") {
        if (args.empty()) {
            spdlog::warn("Bindings: split_pane requires a direction arg (\"right\", \"down\", \"left\", \"up\")");
            return std::nullopt;
        }
        std::string d = toLower(args[0]);
        if (d == "right") return Action::SplitPane{Action::Direction::Right};
        if (d == "down")  return Action::SplitPane{Action::Direction::Down};
        if (d == "left")  return Action::SplitPane{Action::Direction::Left};
        if (d == "up")    return Action::SplitPane{Action::Direction::Up};
        spdlog::warn("Bindings: unknown split_pane direction '{}'", args[0]);
        return std::nullopt;
    }
    if (name == "close_pane") return Action::ClosePane{};
    if (name == "zoom_pane")  return Action::ZoomPane{};

    if (name == "focus_pane") {
        if (args.empty()) {
            spdlog::warn("Bindings: focus_pane requires a direction arg");
            return std::nullopt;
        }
        std::string d = toLower(args[0]);
        if (d == "left")  return Action::FocusPane{Action::Direction::Left};
        if (d == "right") return Action::FocusPane{Action::Direction::Right};
        if (d == "up")    return Action::FocusPane{Action::Direction::Up};
        if (d == "down")  return Action::FocusPane{Action::Direction::Down};
        if (d == "next")  return Action::FocusPane{Action::Direction::Next};
        if (d == "prev")  return Action::FocusPane{Action::Direction::Prev};
        spdlog::warn("Bindings: unknown focus_pane direction '{}'", args[0]);
        return std::nullopt;
    }
    if (name == "adjust_pane_size") {
        if (args.size() < 2) {
            spdlog::warn("Bindings: adjust_pane_size requires direction and amount args");
            return std::nullopt;
        }
        std::string d = toLower(args[0]);
        int amount = 1;
        try { amount = std::stoi(args[1]); } catch (...) {}
        if (d == "left")  return Action::AdjustPaneSize{Action::Direction::Left,  amount};
        if (d == "right") return Action::AdjustPaneSize{Action::Direction::Right, amount};
        if (d == "up")    return Action::AdjustPaneSize{Action::Direction::Up,    amount};
        if (d == "down")  return Action::AdjustPaneSize{Action::Direction::Down,  amount};
        spdlog::warn("Bindings: unknown adjust_pane_size direction '{}'", args[0]);
        return std::nullopt;
    }

    if (name == "copy")  return Action::Copy{};
    if (name == "paste") return Action::Paste{};

    if (name == "scroll_up") {
        int lines = args.empty() ? 3 : std::stoi(args[0]);
        return Action::ScrollUp{lines};
    }
    if (name == "scroll_down") {
        int lines = args.empty() ? 3 : std::stoi(args[0]);
        return Action::ScrollDown{lines};
    }
    if (name == "scroll_to_top")    return Action::ScrollToTop{};
    if (name == "scroll_to_bottom") return Action::ScrollToBottom{};
    if (name == "push_overlay")     return Action::PushOverlay{};
    if (name == "pop_overlay")      return Action::PopOverlay{};
    if (name == "increase_font_size") return Action::IncreaseFontSize{};
    if (name == "decrease_font_size") return Action::DecreaseFontSize{};
    if (name == "reset_font_size")    return Action::ResetFontSize{};
    if (name == "scroll_to_previous_prompt") return Action::ScrollToPrompt{-1};
    if (name == "scroll_to_next_prompt")     return Action::ScrollToPrompt{1};
    if (name == "select_command_output")     return Action::SelectCommandOutput{};
    if (name == "show_scrollback")           return Action::ShowScrollback{};

    spdlog::warn("Bindings: unknown action '{}'", name);
    return std::nullopt;
}

std::vector<Binding> parseBindings(const std::vector<BindingConfig>& configs)
{
    std::vector<Binding> result;
    for (const auto& cfg : configs) {
        if (cfg.keys.empty()) {
            spdlog::warn("Bindings: binding with action '{}' has no keys, skipping", cfg.action);
            continue;
        }
        auto action = parseAction(cfg.action, cfg.args);
        if (!action) continue;

        Binding b;
        b.action = std::move(*action);
        bool ok = true;
        for (const auto& ks : cfg.keys) {
            auto stroke = parseKeyStroke(ks);
            if (!stroke) { ok = false; break; }
            b.keys.push_back(*stroke);
        }
        if (ok) result.push_back(std::move(b));
    }
    return result;
}

std::vector<Binding> defaultBindings()
{
#ifdef __APPLE__
    return {
        // Tab management (Cmd+key on macOS)
        { { *parseKeyStroke("meta+t") },            Action::NewTab{}  },
        { { *parseKeyStroke("meta+w") },            Action::ClosePane{} },
        { { *parseKeyStroke("meta+c") },            Action::Copy{}    },
        { { *parseKeyStroke("meta+v") },            Action::Paste{}   },
        // Tab switching
        { { *parseKeyStroke("meta+shift+]") },      Action::ActivateTabRelative{1}  },
        { { *parseKeyStroke("meta+shift+[") },      Action::ActivateTabRelative{-1} },
        { { *parseKeyStroke("meta+1") },            Action::ActivateTab{0} },
        { { *parseKeyStroke("meta+2") },            Action::ActivateTab{1} },
        { { *parseKeyStroke("meta+3") },            Action::ActivateTab{2} },
        { { *parseKeyStroke("meta+4") },            Action::ActivateTab{3} },
        { { *parseKeyStroke("meta+5") },            Action::ActivateTab{4} },
        { { *parseKeyStroke("meta+6") },            Action::ActivateTab{5} },
        { { *parseKeyStroke("meta+7") },            Action::ActivateTab{6} },
        { { *parseKeyStroke("meta+8") },            Action::ActivateTab{7} },
        { { *parseKeyStroke("meta+9") },            Action::ActivateTab{8} },
        // Font size
        { { *parseKeyStroke("meta+=") },            Action::IncreaseFontSize{} },
        { { *parseKeyStroke("meta+-") },            Action::DecreaseFontSize{} },
        { { *parseKeyStroke("meta+0") },            Action::ResetFontSize{}    },
        // Pane splits
        { { *parseKeyStroke("meta+d") },            Action::SplitPane{Action::Direction::Right} },
        { { *parseKeyStroke("meta+shift+d") },      Action::SplitPane{Action::Direction::Down}  },
        // Pane management
        { { *parseKeyStroke("meta+shift+z") },      Action::ZoomPane{}  },
        // Pane focus — spatial
        { { *parseKeyStroke("meta+alt+left") },     Action::FocusPane{Action::Direction::Left}  },
        { { *parseKeyStroke("meta+alt+right") },    Action::FocusPane{Action::Direction::Right} },
        { { *parseKeyStroke("meta+alt+up") },       Action::FocusPane{Action::Direction::Up}    },
        { { *parseKeyStroke("meta+alt+down") },     Action::FocusPane{Action::Direction::Down}  },
        // Pane focus — cyclic
        { { *parseKeyStroke("meta+]") },            Action::FocusPane{Action::Direction::Next} },
        { { *parseKeyStroke("meta+[") },            Action::FocusPane{Action::Direction::Prev} },
        // Prompt navigation
        { { *parseKeyStroke("meta+up") },           Action::ScrollToPrompt{-1} },
        { { *parseKeyStroke("meta+down") },         Action::ScrollToPrompt{1}  },
        // Scrollback search
        { { *parseKeyStroke("meta+f") },            Action::ShowScrollback{} },
    };
#else
    return {
        // Tab management (Ctrl+Shift+key on Linux)
        { { *parseKeyStroke("ctrl+shift+t") },      Action::NewTab{}  },
        { { *parseKeyStroke("ctrl+shift+w") },      Action::ClosePane{} },
        { { *parseKeyStroke("ctrl+shift+c") },      Action::Copy{}    },
        { { *parseKeyStroke("ctrl+shift+v") },      Action::Paste{}   },
        // Tab switching
        { { *parseKeyStroke("ctrl+shift+pagedown") }, Action::ActivateTabRelative{1}  },
        { { *parseKeyStroke("ctrl+shift+pageup") },   Action::ActivateTabRelative{-1} },
        { { *parseKeyStroke("alt+1") },             Action::ActivateTab{0} },
        { { *parseKeyStroke("alt+2") },             Action::ActivateTab{1} },
        { { *parseKeyStroke("alt+3") },             Action::ActivateTab{2} },
        { { *parseKeyStroke("alt+4") },             Action::ActivateTab{3} },
        { { *parseKeyStroke("alt+5") },             Action::ActivateTab{4} },
        { { *parseKeyStroke("alt+6") },             Action::ActivateTab{5} },
        { { *parseKeyStroke("alt+7") },             Action::ActivateTab{6} },
        { { *parseKeyStroke("alt+8") },             Action::ActivateTab{7} },
        { { *parseKeyStroke("alt+9") },             Action::ActivateTab{8} },
        // Font size
        { { *parseKeyStroke("ctrl+=") },            Action::IncreaseFontSize{} },
        { { *parseKeyStroke("ctrl+-") },            Action::DecreaseFontSize{} },
        { { *parseKeyStroke("ctrl+0") },            Action::ResetFontSize{}    },
        // Pane splits
        { { *parseKeyStroke("ctrl+shift+e") },      Action::SplitPane{Action::Direction::Right} },
        { { *parseKeyStroke("ctrl+shift+o") },      Action::SplitPane{Action::Direction::Down}  },
        // Pane management
        { { *parseKeyStroke("ctrl+shift+z") },      Action::ZoomPane{}  },
        // Pane focus — spatial
        { { *parseKeyStroke("ctrl+shift+left") },   Action::FocusPane{Action::Direction::Left}  },
        { { *parseKeyStroke("ctrl+shift+right") },  Action::FocusPane{Action::Direction::Right} },
        { { *parseKeyStroke("ctrl+shift+up") },     Action::FocusPane{Action::Direction::Up}    },
        { { *parseKeyStroke("ctrl+shift+down") },   Action::FocusPane{Action::Direction::Down}  },
        // Pane focus — cyclic
        { { *parseKeyStroke("ctrl+shift+n") },      Action::FocusPane{Action::Direction::Next} },
        { { *parseKeyStroke("ctrl+shift+p") },      Action::FocusPane{Action::Direction::Prev} },
        // Prompt navigation
        { { *parseKeyStroke("ctrl+alt+z") },        Action::ScrollToPrompt{-1} },
        { { *parseKeyStroke("ctrl+alt+x") },        Action::ScrollToPrompt{1}  },
        // Scrollback search
        { { *parseKeyStroke("ctrl+shift+f") },      Action::ShowScrollback{} },
    };
#endif
}

SequenceMatcher::MatchResult SequenceMatcher::advance(
    const KeyStroke& ks, const std::vector<Binding>& bindings)
{
    current_.push_back(ks);

    const Action::Any* exactMatch = nullptr;
    bool hasPrefix = false;

    for (const auto& b : bindings) {
        if (b.keys.size() < current_.size()) continue;
        bool match = true;
        for (size_t i = 0; i < current_.size(); ++i) {
            if (!(b.keys[i] == current_[i])) { match = false; break; }
        }
        if (!match) continue;
        if (b.keys.size() == current_.size()) exactMatch = &b.action;
        else                                   hasPrefix  = true;
    }

    if (exactMatch) {
        Action::Any action = *exactMatch;
        reset();
        return {Result::Match, std::move(action)};
    }
    if (hasPrefix) return {Result::Prefix, std::nullopt};
    reset();
    return {Result::NoMatch, std::nullopt};
}

void SequenceMatcher::reset()
{
    current_.clear();
}
