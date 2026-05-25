#include "Bindings.h"
#include "Config.h"
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   {
                       return std::tolower(c);
                   });
    return s;
}

// snake_case → PascalCase. Inverse of ScriptEngine.cpp's toSnakeCase.
// "activate_tab_relative" → "ActivateTabRelative".
static std::string snakeToPascal(std::string_view snake)
{
    std::string out;
    out.reserve(snake.size());
    bool capNext = true;
    for (char c : snake) {
        if (c == '_') {
            capNext = true;
            continue;
        }
        if (capNext) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capNext = false;
        } else {
            out += c;
        }
    }
    return out;
}

// Aliases for action names that aren't a clean snake_case translation
// of the Pascal variant. Each entry maps an alias (TOML / JS-visible
// name) to (Pascal name, baked positional args that complete the
// invocation). The baked args are concatenated *before* user-supplied
// args, so e.g. `scroll_to_previous_prompt` resolves to
// (`ScrollToPrompt`, {"prev"}) and any user args follow — which is
// vacuous for these zero-arg aliases.
struct ActionAlias
{
    std::string_view pascal;
    std::vector<std::string> bakedArgs;
};

static const std::unordered_map<std::string, ActionAlias> &actionAliases()
{
    static const std::unordered_map<std::string, ActionAlias> m = {
        { "scroll_to_previous_prompt", { "ScrollToPrompt", { "prev" } } },
        { "scroll_to_next_prompt", { "ScrollToPrompt", { "next" } } },
    };
    return m;
}

// One-time wiring: hand Action.cpp our keystroke parser so the SendKey
// schema-driven builder can resolve "ctrl+a" → Key+mods without
// importing Bindings into Action.cpp.
namespace {
struct SendKeyParserInit
{
    SendKeyParserInit()
    {
        Action::setSendKeyParser([](const std::string &ks) -> Action::SendKey
                                 {
                                     auto parsed = parseKeyStroke(ks);
                                     if (!parsed) {
                                         return Action::SendKey { Key_unknown, 0 };
                                     }
                                     return Action::SendKey { parsed->key, parsed->mods };
                                 });
    }
} sendKeyParserInit;
} // namespace

std::optional<KeyStroke> parseKeyStroke(const std::string &s)
{
    // Split on '+', last token is the key name, earlier tokens are modifiers.
    std::vector<std::string> parts;
    std::string token;
    for (char c : s) {
        if (c == '+') {
            if (!token.empty()) {
                parts.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) {
        parts.push_back(token);
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    std::string keyStr = parts.back();
    parts.pop_back();

    uint32_t mods = 0;
    for (const auto &mod : parts) {
        std::string m = toLower(mod);
        if (m == "ctrl" || m == "control") {
            mods |= CtrlModifier;
        } else if (m == "shift") {
            mods |= ShiftModifier;
        } else if (m == "alt") {
            mods |= AltModifier;
        } else if (m == "meta" || m == "super" || m == "cmd" || m == "win") {
            mods |= MetaModifier;
        } else {
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
            { "escape", Key_Escape },
            { "tab", Key_Tab },
            { "backspace", Key_Backspace },
            { "return", Key_Return },
            { "enter", Key_Return },
            { "space", Key_Space },
            { "left", Key_Left },
            { "right", Key_Right },
            { "up", Key_Up },
            { "down", Key_Down },
            { "home", Key_Home },
            { "end", Key_End },
            { "pageup", Key_PageUp },
            { "pagedown", Key_PageDown },
            { "insert", Key_Insert },
            { "delete", Key_Delete },
            { "f1", Key_F1 },
            { "f2", Key_F2 },
            { "f3", Key_F3 },
            { "f4", Key_F4 },
            { "f5", Key_F5 },
            { "f6", Key_F6 },
            { "f7", Key_F7 },
            { "f8", Key_F8 },
            { "f9", Key_F9 },
            { "f10", Key_F10 },
            { "f11", Key_F11 },
            { "f12", Key_F12 },
        };
        auto it = keyMap.find(ks);
        if (it == keyMap.end()) {
            spdlog::warn("Bindings: unknown key '{}' in '{}'", keyStr, s);
            return std::nullopt;
        }
        key = it->second;
    }

    return KeyStroke { key, mods };
}

std::optional<Action::Any> parseAction(const std::string &name,
                                       const std::vector<std::string> &args)
{
    // Script actions: any name containing a '.' is treated as
    // namespace.action — never goes through the schema registry. The
    // args are forwarded verbatim; the script side validates against
    // its own (optional) schema at dispatch time.
    if (name.find('.') != std::string::npos) {
        return Action::ScriptAction { name, args };
    }

    // Resolve aliases (e.g. scroll_to_previous_prompt → ScrollToPrompt
    // with "prev" baked in). Aliases supply additional positional
    // args that prepend the caller's args before coercion.
    std::vector<std::string> effectiveArgs = args;
    std::string pascal;
    if (auto it = actionAliases().find(name); it != actionAliases().end()) {
        pascal                            = std::string(it->second.pascal);
        std::vector<std::string> combined = it->second.bakedArgs;
        combined.insert(combined.end(), args.begin(), args.end());
        effectiveArgs = std::move(combined);
    } else {
        pascal = snakeToPascal(name);
    }

    // Delegate to the schema-driven typed builder. The adapter applies
    // ArgKind coercion (Int parse, Direction validate, etc.) and
    // calls the variant constructor co-located with each schema in
    // Action.cpp.
    std::string error;
    auto built = Action::buildActionFromPositional(pascal, effectiveArgs, error);
    if (!built) {
        spdlog::warn("Bindings: {}", error);
        return std::nullopt;
    }
    return built;
}

std::optional<Action::Any> parseActionTyped(const std::string &snakeName,
                                            const Action::ArgsValue &args)
{
    // Script actions pass through verbatim with their args flattened
    // into a positional string list (the legacy ScriptAction wire
    // format). Script actions with a JS-side schema are handled on
    // the JS side (engine builds the object-arg payload before
    // delivering to the JS handler); the C++ ScriptAction variant
    // doesn't need to know about it.
    if (snakeName.find('.') != std::string::npos) {
        std::vector<std::string> positional;
        positional.reserve(args.size());
        for (const auto &[k, v] : args) {
            if (auto *s = v.template get_if<std::string>()) {
                positional.push_back(*s);
            } else if (auto *i = v.template get_if<int64_t>()) {
                positional.push_back(std::to_string(*i));
            } else if (auto *b = v.template get_if<bool>()) {
                positional.push_back(*b ? "true" : "false");
            }
        }
        return Action::ScriptAction { snakeName, std::move(positional) };
    }
    std::string pascal;
    if (auto it = actionAliases().find(snakeName); it != actionAliases().end()) {
        pascal                              = std::string(it->second.pascal);
        // Aliased actions baked positional values; merge them into
        // the typed args, treating each as the schema's first N args
        // by position. Rare path; just go through the positional
        // adapter for these so we don't fight glaze types here.
        std::vector<std::string> positional = it->second.bakedArgs;
        for (const auto &[k, v] : args) {
            if (auto *s = v.template get_if<std::string>()) {
                positional.push_back(*s);
            }
        }
        std::string error;
        auto built = Action::buildActionFromPositional(pascal, positional, error);
        if (!built) {
            spdlog::warn("Bindings: {}", error);
            return std::nullopt;
        }
        return built;
    }
    pascal = snakeToPascal(snakeName);
    std::string error;
    auto built = Action::buildActionFromArgs(pascal, args, error);
    if (!built) {
        spdlog::warn("Bindings: {}", error);
        return std::nullopt;
    }
    return built;
}

std::optional<MouseAction::Any> parseMouseAction(const std::string &name,
                                                 const std::vector<std::string> &args)
{
    if (name == "mouse_selection") {
        using S = MouseAction::SelectionType;
        if (args.empty()) {
            return MouseAction::StartSelection { S::Normal };
        }
        std::string m = toLower(args[0]);
        if (m == "normal") {
            return MouseAction::StartSelection { S::Normal };
        }
        if (m == "word") {
            return MouseAction::StartSelection { S::Word };
        }
        if (m == "line") {
            return MouseAction::StartSelection { S::Line };
        }
        if (m == "extend") {
            return MouseAction::StartSelection { S::Extend };
        }
        if (m == "rectangle") {
            return MouseAction::StartSelection { S::Rectangle };
        }
        spdlog::warn("Bindings: unknown mouse_selection mode '{}'", args[0]);
        return std::nullopt;
    }
    if (name == "open_hyperlink") {
        return MouseAction::OpenHyperlink {};
    }
    if (name == "select_command") {
        return MouseAction::SelectCommand {};
    }
    return std::nullopt;
}

// Convert a glz::generic scalar (loaded from a TOML table-form `args`
// entry) into an Action::ArgValue. TOML scalars come through as
// glz::json_t's `data` variant: string, double (TOML int/float collapse
// to double in glaze's generic), bool, null. Arrays and nested objects
// are not supported in action args yet; if encountered they collapse
// to a string of the JSON form (best-effort, mostly for forward
// compat).
static Action::ArgValue genericToArg(const glz::generic &g)
{
    using Var = glz::generic::val_t;
    return std::visit([](const auto &v) -> Action::ArgValue
                      {
                          using T = std::decay_t<decltype(v)>;
                          if constexpr (std::is_same_v<T, std::nullptr_t>) {
                              return Action::ArgValue {};
                          } else if constexpr (std::is_same_v<T, bool>) {
                              return Action::ArgValue { v };
                          } else if constexpr (std::is_same_v<T, double>) {
                              // TOML ints arrive as doubles through glaze's generic.
                              // Cast to int64_t when integral; otherwise carry as
                              // double (the schema's ArgKind decides reinterpretation
                              // at coerce time).
                              double d = v;
                              if (d == static_cast<int64_t>(d)) {
                                  return Action::ArgValue { static_cast<int64_t>(d) };
                              }
                              return Action::ArgValue { d };
                          } else if constexpr (std::is_same_v<T, std::string>) {
                              return Action::ArgValue { v };
                          } else {
                              // Arrays, objects — collapse to a JSON-ish string. Not
                              // a real use case for current actions; keeps the path
                              // total instead of dropping silently.
                              std::string s;
                              (void)glz::write_json(v, s);
                              return Action::ArgValue { s };
                          }
                      },
                      g.data);
}

// Translate a TOML BindingArgs into either positional strings or a
// typed ArgsValue, then through to a typed Action::Any.
static std::optional<Action::Any> parseActionFromBindingArgs(const std::string &name,
                                                             const BindingArgs &args)
{
    if (auto *pos = std::get_if<std::vector<std::string>>(&args)) {
        return parseAction(name, *pos);
    }
    const auto &tbl = std::get<BindingArgsTable>(args);
    Action::ArgsValue typed;
    typed.reserve(tbl.size());
    for (const auto &[k, v] : tbl) {
        typed.emplace(k, genericToArg(v));
    }
    return parseActionTyped(name, typed);
}

// Flatten BindingArgs into positional strings for callers that still
// require the old shape (parseMouseAction's mouse-only action coercer,
// which has its own micro-grammar and hasn't moved to schemas yet).
static std::vector<std::string> bindingArgsToPositional(const BindingArgs &args)
{
    if (auto *pos = std::get_if<std::vector<std::string>>(&args)) {
        return *pos;
    }
    // Named form to positional: dump string values in declaration
    // order. This is best-effort for the mouse-action path which
    // typically takes 0-1 args.
    const auto &tbl = std::get<BindingArgsTable>(args);
    std::vector<std::string> out;
    out.reserve(tbl.size());
    for (const auto &[k, v] : tbl) {
        if (auto *s = std::get_if<std::string>(&v.data)) {
            out.push_back(*s);
        }
    }
    return out;
}

std::vector<Binding> parseBindings(const std::vector<BindingConfig> &configs)
{
    std::vector<Binding> result;
    for (const auto &cfg : configs) {
        if (cfg.keys.empty()) {
            spdlog::warn("Bindings: binding with action '{}' has no keys, skipping", cfg.action);
            continue;
        }
        auto action = parseActionFromBindingArgs(cfg.action, cfg.args);
        if (!action) {
            continue;
        }

        Binding b;
        b.action = std::move(*action);
        bool ok  = true;
        for (const auto &ks : cfg.keys) {
            auto stroke = parseKeyStroke(ks);
            if (!stroke) {
                ok = false;
                break;
            }
            b.keys.push_back(*stroke);
        }
        if (ok) {
            result.push_back(std::move(b));
        }
    }
    return result;
}

std::vector<Binding> defaultBindings()
{
#ifdef __APPLE__
    return {
        // Tab management (Cmd+key on macOS)
        { { *parseKeyStroke("meta+t") }, Action::NewTab {} },
        { { *parseKeyStroke("meta+w") }, Action::ClosePane {} },
        { { *parseKeyStroke("meta+c") }, Action::Copy {} },
        { { *parseKeyStroke("meta+v") }, Action::Paste {} },
        // Tab switching
        { { *parseKeyStroke("meta+shift+]") }, Action::ActivateTabRelative { Uuid {}, 1 } },
        { { *parseKeyStroke("meta+shift+[") }, Action::ActivateTabRelative { Uuid {}, -1 } },
        { { *parseKeyStroke("meta+1") }, Action::ActivateTab { Uuid {}, 0 } },
        { { *parseKeyStroke("meta+2") }, Action::ActivateTab { Uuid {}, 1 } },
        { { *parseKeyStroke("meta+3") }, Action::ActivateTab { Uuid {}, 2 } },
        { { *parseKeyStroke("meta+4") }, Action::ActivateTab { Uuid {}, 3 } },
        { { *parseKeyStroke("meta+5") }, Action::ActivateTab { Uuid {}, 4 } },
        { { *parseKeyStroke("meta+6") }, Action::ActivateTab { Uuid {}, 5 } },
        { { *parseKeyStroke("meta+7") }, Action::ActivateTab { Uuid {}, 6 } },
        { { *parseKeyStroke("meta+8") }, Action::ActivateTab { Uuid {}, 7 } },
        { { *parseKeyStroke("meta+9") }, Action::ActivateTab { Uuid {}, 8 } },
        // Font size
        { { *parseKeyStroke("meta+=") }, Action::IncreaseFontSize {} },
        { { *parseKeyStroke("meta+-") }, Action::DecreaseFontSize {} },
        { { *parseKeyStroke("meta+0") }, Action::ResetFontSize {} },
        // Pane splits
        { { *parseKeyStroke("meta+d") }, Action::SplitPane { Action::Direction::Right } },
        { { *parseKeyStroke("meta+shift+d") }, Action::SplitPane { Action::Direction::Down } },
        // Pane management
        { { *parseKeyStroke("meta+shift+z") }, Action::ZoomPane {} },
        // Pane focus — spatial
        { { *parseKeyStroke("meta+alt+left") }, Action::FocusPane { Action::Direction::Left } },
        { { *parseKeyStroke("meta+alt+right") }, Action::FocusPane { Action::Direction::Right } },
        { { *parseKeyStroke("meta+alt+up") }, Action::FocusPane { Action::Direction::Up } },
        { { *parseKeyStroke("meta+alt+down") }, Action::FocusPane { Action::Direction::Down } },
        // Pane focus — cyclic
        { { *parseKeyStroke("meta+]") }, Action::FocusPane { Action::Direction::Next } },
        { { *parseKeyStroke("meta+[") }, Action::FocusPane { Action::Direction::Prev } },
        // Command palette
        { { *parseKeyStroke("meta+shift+p") }, Action::ScriptAction { "palette.open", {} } },
        // Popup focus cycling
        { { *parseKeyStroke("meta+shift+i") }, Action::FocusPopup {} },
        // Prompt navigation
        { { *parseKeyStroke("meta+up") }, Action::ScrollToPrompt { -1 } },
        { { *parseKeyStroke("meta+down") }, Action::ScrollToPrompt { 1 } },
        // Scrollback page navigation
        { { *parseKeyStroke("ctrl+pageup") }, Action::ScrollPageUp {} },
        { { *parseKeyStroke("ctrl+pagedown") }, Action::ScrollPageDown {} },
        // Scrollback search (in-pane, decoration-overlay-driven). The
        // built-in `scrollback-search.js` applet registers
        // `search.open`; opening it on a pane that already has a search
        // popup toggles it closed.
        { { *parseKeyStroke("meta+f") }, Action::ScriptAction { "search.open", {} } },
        // Set custom pane title. The built-in `set-title.js` applet
        // registers `pane.set_title`; a centered popup edits an override
        // that takes precedence over OSC 0/2. Toggles closed on
        // re-invocation while open.
        { { *parseKeyStroke("meta+alt+t") }, Action::ScriptAction { "pane.set_title", {} } },
        // Tab / pane reorder
        { { *parseKeyStroke("meta+shift+pageup") }, Action::MoveTab { -1 } },
        { { *parseKeyStroke("meta+shift+pagedown") }, Action::MoveTab { +1 } },
        { { *parseKeyStroke("meta+ctrl+left") }, Action::SwapPane { Action::Direction::Left } },
        { { *parseKeyStroke("meta+ctrl+right") }, Action::SwapPane { Action::Direction::Right } },
        { { *parseKeyStroke("meta+ctrl+up") }, Action::SwapPane { Action::Direction::Up } },
        { { *parseKeyStroke("meta+ctrl+down") }, Action::SwapPane { Action::Direction::Down } },
        { { *parseKeyStroke("meta+alt+n") }, Action::SwapPane { Action::Direction::Next } },
        { { *parseKeyStroke("meta+alt+p") }, Action::SwapPane { Action::Direction::Prev } },
        { { *parseKeyStroke("meta+alt+r") }, Action::RotatePanes { +1 } },
        { { *parseKeyStroke("meta+alt+shift+r") }, Action::RotatePanes { -1 } },
    };
#else
    return {
        // Tab management (Ctrl+Shift+key on Linux)
        { { *parseKeyStroke("ctrl+shift+t") }, Action::NewTab {} },
        { { *parseKeyStroke("ctrl+shift+w") }, Action::ClosePane {} },
        { { *parseKeyStroke("ctrl+shift+c") }, Action::Copy {} },
        { { *parseKeyStroke("ctrl+shift+v") }, Action::Paste {} },
        // Tab switching.
        //
        // ActivateTab/ActivateTabRelative MUST be brace-initialized with
        // both fields (`Uuid {}, N`). Writing `ActivateTab { N }` is an
        // aggregate-init landmine: since Uuid is itself an aggregate
        // ({uint64 high, uint64 low}), `{N}` initializes `target.high = N`
        // and leaves `index` at its default (-1), producing a binding that
        // resolves to "no specific tab" at dispatch time. The same shape
        // applies to ActivateTabRelative (Uuid stack + int delta).
        { { *parseKeyStroke("ctrl+shift+pagedown") }, Action::ActivateTabRelative { Uuid {}, 1 } },
        { { *parseKeyStroke("ctrl+shift+pageup") }, Action::ActivateTabRelative { Uuid {}, -1 } },
        { { *parseKeyStroke("alt+shift+right") }, Action::ActivateTabRelative { Uuid {}, 1 } },
        { { *parseKeyStroke("alt+shift+left") }, Action::ActivateTabRelative { Uuid {}, -1 } },
        { { *parseKeyStroke("alt+1") }, Action::ActivateTab { Uuid {}, 0 } },
        { { *parseKeyStroke("alt+2") }, Action::ActivateTab { Uuid {}, 1 } },
        { { *parseKeyStroke("alt+3") }, Action::ActivateTab { Uuid {}, 2 } },
        { { *parseKeyStroke("alt+4") }, Action::ActivateTab { Uuid {}, 3 } },
        { { *parseKeyStroke("alt+5") }, Action::ActivateTab { Uuid {}, 4 } },
        { { *parseKeyStroke("alt+6") }, Action::ActivateTab { Uuid {}, 5 } },
        { { *parseKeyStroke("alt+7") }, Action::ActivateTab { Uuid {}, 6 } },
        { { *parseKeyStroke("alt+8") }, Action::ActivateTab { Uuid {}, 7 } },
        { { *parseKeyStroke("alt+9") }, Action::ActivateTab { Uuid {}, 8 } },
        // Font size
        { { *parseKeyStroke("ctrl+=") }, Action::IncreaseFontSize {} },
        { { *parseKeyStroke("ctrl+-") }, Action::DecreaseFontSize {} },
        { { *parseKeyStroke("ctrl+0") }, Action::ResetFontSize {} },
        // Pane splits
        { { *parseKeyStroke("ctrl+shift+e") }, Action::SplitPane { Action::Direction::Right } },
        { { *parseKeyStroke("ctrl+shift+o") }, Action::SplitPane { Action::Direction::Down } },
        // Pane management
        { { *parseKeyStroke("ctrl+shift+z") }, Action::ZoomPane {} },
        // Pane focus — spatial
        { { *parseKeyStroke("ctrl+shift+left") }, Action::FocusPane { Action::Direction::Left } },
        { { *parseKeyStroke("ctrl+shift+right") }, Action::FocusPane { Action::Direction::Right } },
        { { *parseKeyStroke("ctrl+shift+up") }, Action::FocusPane { Action::Direction::Up } },
        { { *parseKeyStroke("ctrl+shift+down") }, Action::FocusPane { Action::Direction::Down } },
        // Pane focus — cyclic
        { { *parseKeyStroke("ctrl+shift+n") }, Action::FocusPane { Action::Direction::Next } },
        { { *parseKeyStroke("ctrl+shift+p") }, Action::FocusPane { Action::Direction::Prev } },
        // Command palette
        { { *parseKeyStroke("ctrl+shift+p") }, Action::ScriptAction { "palette.open", {} } },
        // Popup focus cycling
        { { *parseKeyStroke("ctrl+shift+i") }, Action::FocusPopup {} },
        // Prompt navigation
        { { *parseKeyStroke("ctrl+alt+z") }, Action::ScrollToPrompt { -1 } },
        { { *parseKeyStroke("ctrl+alt+x") }, Action::ScrollToPrompt { 1 } },
        // Scrollback page navigation
        { { *parseKeyStroke("ctrl+pageup") }, Action::ScrollPageUp {} },
        { { *parseKeyStroke("ctrl+pagedown") }, Action::ScrollPageDown {} },
        // Scrollback search (in-pane, decoration-overlay-driven). The
        // built-in `scrollback-search.js` applet registers
        // `search.open`; opening it on a pane that already has a search
        // popup toggles it closed.
        { { *parseKeyStroke("ctrl+shift+f") }, Action::ScriptAction { "search.open", {} } },
        // Set custom pane title. The built-in `set-title.js` applet
        // registers `pane.set_title`; a centered popup edits an override
        // that takes precedence over OSC 0/2. Toggles closed on
        // re-invocation while open.
        { { *parseKeyStroke("ctrl+alt+t") }, Action::ScriptAction { "pane.set_title", {} } },
        // Tab / pane reorder
        { { *parseKeyStroke("ctrl+alt+pageup") }, Action::MoveTab { -1 } },
        { { *parseKeyStroke("ctrl+alt+pagedown") }, Action::MoveTab { +1 } },
        { { *parseKeyStroke("ctrl+alt+left") }, Action::SwapPane { Action::Direction::Left } },
        { { *parseKeyStroke("ctrl+alt+right") }, Action::SwapPane { Action::Direction::Right } },
        { { *parseKeyStroke("ctrl+alt+up") }, Action::SwapPane { Action::Direction::Up } },
        { { *parseKeyStroke("ctrl+alt+down") }, Action::SwapPane { Action::Direction::Down } },
        { { *parseKeyStroke("ctrl+alt+n") }, Action::SwapPane { Action::Direction::Next } },
        { { *parseKeyStroke("ctrl+alt+p") }, Action::SwapPane { Action::Direction::Prev } },
        { { *parseKeyStroke("ctrl+alt+r") }, Action::RotatePanes { +1 } },
        { { *parseKeyStroke("ctrl+alt+shift+r") }, Action::RotatePanes { -1 } },
    };
#endif
}

std::vector<Binding> mergeKeyBindings(std::vector<Binding> defaults,
                                      std::vector<Binding> user)
{
    std::vector<Binding> result;
    result.reserve(defaults.size() + user.size());
    for (auto &d : defaults) {
        bool shadowed = false;
        for (const auto &u : user) {
            if (d.keys == u.keys) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) {
            result.push_back(std::move(d));
        }
    }
    // DisableDefaultAssignment is a marker that drops the matching default and
    // leaves the stroke unbound (falls through to PTY). The shadowing pass
    // above already removed the default; here we just skip emitting the user
    // entry so the stroke isn't bound to anything.
    for (auto &u : user) {
        if (std::holds_alternative<Action::DisableDefaultAssignment>(u.action)) {
            continue;
        }
        result.push_back(std::move(u));
    }
    return result;
}

SequenceMatcher::MatchResult SequenceMatcher::advance(
    const KeyStroke &ks, const std::vector<Binding> &bindings)
{
    current_.push_back(ks);

    std::vector<Action::Any> exactMatches;
    bool hasPrefix = false;

    for (const auto &b : bindings) {
        if (b.keys.size() < current_.size()) {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < current_.size(); ++i) {
            if (!(b.keys[i] == current_[i])) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        if (b.keys.size() == current_.size()) {
            exactMatches.push_back(b.action);
        } else {
            hasPrefix = true;
        }
    }

    if (!exactMatches.empty()) {
        reset();
        return { Result::Match, std::move(exactMatches), {} };
    }
    if (hasPrefix) {
        return { Result::Prefix, {}, {} };
    }
    // NoMatch: hand back the prefix keys that got swallowed before this one
    // (current_ still includes the failing key at its tail; exclude it).
    std::vector<KeyStroke> aborted;
    if (current_.size() > 1) {
        aborted.assign(current_.begin(), current_.end() - 1);
    }
    reset();
    return { Result::NoMatch, {}, std::move(aborted) };
}

void SequenceMatcher::reset()
{
    current_.clear();
}

// ============================================================================
// Mouse bindings
// ============================================================================

bool MouseStroke::matches(const MouseStroke &incoming) const noexcept
{
    if (button != incoming.button) {
        return false;
    }
    if (mods != incoming.mods) {
        return false;
    }
    if (event != incoming.event) {
        return false;
    }
    // Mode: Any matches both, otherwise must match exactly
    if (mode != MouseMode::Any && incoming.mode != MouseMode::Any && mode != incoming.mode) {
        return false;
    }
    // Region: Any matches all, otherwise must match exactly
    if (region != MouseRegion::Any && incoming.region != MouseRegion::Any && region != incoming.region) {
        return false;
    }
    return true;
}

static MouseButton parseMouseButton(const std::string &s)
{
    std::string b = toLower(s);
    if (b == "middle" || b == "mid") {
        return MouseButton::Middle;
    }
    if (b == "right") {
        return MouseButton::Right;
    }
    return MouseButton::Left;
}

static MouseEventType parseMouseEventType(const std::string &s)
{
    std::string e = toLower(s);
    if (e == "release") {
        return MouseEventType::Release;
    }
    if (e == "click") {
        return MouseEventType::Click;
    }
    if (e == "doublepress") {
        return MouseEventType::DoublePress;
    }
    if (e == "triplepress") {
        return MouseEventType::TriplePress;
    }
    if (e == "drag") {
        return MouseEventType::Drag;
    }
    return MouseEventType::Press;
}

static MouseMode parseMouseMode(const std::string &s)
{
    std::string m = toLower(s);
    if (m == "grabbed") {
        return MouseMode::Grabbed;
    }
    if (m == "any") {
        return MouseMode::Any;
    }
    return MouseMode::Ungrabbed;
}

static MouseRegion parseMouseRegion(const std::string &s)
{
    std::string r = toLower(s);
    if (r == "tab_bar" || r == "tabbar") {
        return MouseRegion::TabBar;
    }
    if (r == "pane") {
        return MouseRegion::Pane;
    }
    if (r == "divider") {
        return MouseRegion::Divider;
    }
    return MouseRegion::Any;
}

std::vector<MouseBinding> parseMouseBindings(const std::vector<MouseBindingConfig> &configs)
{
    std::vector<MouseBinding> result;
    for (const auto &cfg : configs) {
        if (cfg.button.empty() || cfg.action.empty()) {
            spdlog::warn("MouseBindings: binding missing button or action, skipping");
            continue;
        }

        // Parse modifiers from button string (e.g. "ctrl+left" → mods + button)
        uint32_t mods         = 0;
        std::string buttonStr = cfg.button;
        // Check for modifier prefixes
        auto plus             = buttonStr.rfind('+');
        if (plus != std::string::npos) {
            std::string modStr = buttonStr.substr(0, plus);
            buttonStr          = buttonStr.substr(plus + 1);
            // Parse modifiers (same as key modifiers)
            std::istringstream ms(modStr);
            std::string mod;
            while (std::getline(ms, mod, '+')) {
                std::string m = toLower(mod);
                if (m == "ctrl" || m == "control") {
                    mods |= CtrlModifier;
                } else if (m == "shift") {
                    mods |= ShiftModifier;
                } else if (m == "alt") {
                    mods |= AltModifier;
                } else if (m == "meta" || m == "super" || m == "cmd") {
                    mods |= MetaModifier;
                }
            }
        }

        // Mouse-only names (mouse_selection, open_hyperlink,
        // select_command) resolve via parseMouseAction; everything
        // else is a keyboard-style Action::Any via parseAction. The
        // split keeps cell-context-bearing types separate from the
        // generic dispatcher's input. parseMouseAction still takes
        // positional args (the mouse-only grammar hasn't been moved
        // to schemas yet — see TODO.md); flatten the BindingArgs
        // variant for it.
        MouseBindingResult result_action;
        std::vector<std::string> positional = bindingArgsToPositional(cfg.args);
        if (auto ma = parseMouseAction(cfg.action, positional)) {
            result_action = *ma;
        } else if (auto ka = parseActionFromBindingArgs(cfg.action, cfg.args)) {
            result_action = *ka;
        } else {
            continue;
        }

        MouseStroke stroke;
        stroke.button = parseMouseButton(buttonStr);
        stroke.mods   = mods;
        stroke.event  = parseMouseEventType(cfg.event.empty() ? "press" : cfg.event);
        stroke.mode   = parseMouseMode(cfg.mode.empty() ? "ungrabbed" : cfg.mode);
        stroke.region = parseMouseRegion(cfg.region.empty() ? "any" : cfg.region);

        result.push_back({ stroke, std::move(result_action) });
    }
    return result;
}

std::vector<MouseBinding> defaultMouseBindings()
{
    using S = MouseAction::SelectionType;
    return {
        // Character selection
        { { MouseButton::Left, 0, MouseEventType::Press, MouseMode::Ungrabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Normal } },
        // Word selection (double-click)
        { { MouseButton::Left, 0, MouseEventType::DoublePress, MouseMode::Ungrabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Word } },
        // Line selection (triple-click)
        { { MouseButton::Left, 0, MouseEventType::TriplePress, MouseMode::Ungrabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Line } },
        // Extend selection
        { { MouseButton::Left, ShiftModifier, MouseEventType::Press, MouseMode::Ungrabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Extend } },
        // Rectangle selection
        { { MouseButton::Left, AltModifier, MouseEventType::Press, MouseMode::Ungrabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Rectangle } },

        // Shift-override in grabbed mode
        { { MouseButton::Left, ShiftModifier, MouseEventType::Press, MouseMode::Grabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Normal } },
        { { MouseButton::Left, ShiftModifier, MouseEventType::DoublePress, MouseMode::Grabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Word } },
        { { MouseButton::Left, ShiftModifier, MouseEventType::TriplePress, MouseMode::Grabbed, MouseRegion::Pane },
          MouseAction::StartSelection { S::Line } },

        // Middle-click paste (Click = single press+release without drag)
        { { MouseButton::Middle, 0, MouseEventType::Click, MouseMode::Ungrabbed, MouseRegion::Pane },
          Action::PasteSelection {} },

    // Hyperlink open + OSC 133 command select on the same gesture. Both
    // fire; OpenHyperlink opens a URL when one is under the cursor (and
    // nothing else on a plain row), SelectCommand sets the highlight
    // unconditionally.
#ifdef __APPLE__
        { { MouseButton::Left, MetaModifier, MouseEventType::Click, MouseMode::Any, MouseRegion::Pane },
          MouseAction::OpenHyperlink {} },
        { { MouseButton::Left, MetaModifier, MouseEventType::Click, MouseMode::Any, MouseRegion::Pane },
          MouseAction::SelectCommand {} },
        // Cmd+double-click promotes the OSC 133 outline into a real text selection
        // over the clicked command's output rows (and auto-copies to clipboard).
        { { MouseButton::Left, MetaModifier, MouseEventType::DoublePress, MouseMode::Any, MouseRegion::Pane },
          Action::SelectCommandOutput {} },
#else
        { { MouseButton::Left, CtrlModifier, MouseEventType::Click, MouseMode::Any, MouseRegion::Pane },
          MouseAction::OpenHyperlink {} },
        { { MouseButton::Left, CtrlModifier, MouseEventType::Click, MouseMode::Any, MouseRegion::Pane },
          MouseAction::SelectCommand {} },
        { { MouseButton::Left, CtrlModifier, MouseEventType::DoublePress, MouseMode::Any, MouseRegion::Pane },
          Action::SelectCommandOutput {} },
#endif

        // Tab bar
        { { MouseButton::Left, 0, MouseEventType::Press, MouseMode::Any, MouseRegion::TabBar },
          Action::ActivateTab { Uuid {}, -1 } },
        { { MouseButton::Middle, 0, MouseEventType::Press, MouseMode::Any, MouseRegion::TabBar },
          Action::CloseTab { Uuid {}, -1 } },
    };
}

std::vector<MouseBindingResult> matchMouseBindings(const MouseStroke &stroke,
                                                   const std::vector<MouseBinding> &bindings)
{
    std::vector<MouseBindingResult> result;
    for (const auto &b : bindings) {
        if (b.trigger.matches(stroke)) {
            result.push_back(b.action);
        }
    }
    return result;
}

std::vector<MouseBinding> mergeMouseBindings(std::vector<MouseBinding> defaults,
                                             std::vector<MouseBinding> user)
{
    std::unordered_set<std::size_t> userActionTypes;
    userActionTypes.reserve(user.size());
    for (const auto &u : user) {
        userActionTypes.insert(mouseBindingTypeKey(u.action));
    }

    std::vector<MouseBinding> result;
    result.reserve(defaults.size() + user.size());
    for (auto &d : defaults) {
        if (userActionTypes.count(mouseBindingTypeKey(d.action))) {
            continue;
        }
        result.push_back(std::move(d));
    }
    for (auto &u : user) {
        result.push_back(std::move(u));
    }
    return result;
}
