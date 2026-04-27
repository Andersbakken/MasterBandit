#pragma once
#include "Action.h"
#include "MouseAction.h"
#include "InputTypes.h"
#include <optional>
#include <string>
#include <vector>

struct BindingConfig; // defined in Config.h

// Normalize left/right modifier keys to their generic form for binding matching
inline Key normalizeModifierKey(Key k)
{
    switch (k) {
    case Key_Shift_L: case Key_Shift_R: return Key_Shift;
    case Key_Control_L: case Key_Control_R: return Key_Control;
    case Key_Alt_L: case Key_Alt_R: return Key_Alt;
    case Key_Super_L: case Key_Super_R: return Key_Meta;
    default: return k;
    }
}

struct KeyStroke {
    Key      key  = Key_unknown;
    uint32_t mods = 0;

    bool operator==(const KeyStroke& o) const noexcept
    {
        return normalizeModifierKey(key) == normalizeModifierKey(o.key) && mods == o.mods;
    }
};

struct Binding {
    std::vector<KeyStroke> keys;
    Action::Any            action;
};

// Parse a single key string like "ctrl+shift+t" → KeyStroke.
std::optional<KeyStroke>   parseKeyStroke(const std::string& s);

// Parse an action name + args → Action::Any.
std::optional<Action::Any> parseAction(const std::string& name,
                                        const std::vector<std::string>& args);

// Parse a mouse-only action name + args → MouseAction::Any. Recognizes the
// names that need cell context: "mouse_selection [normal|word|line|extend|
// rectangle]", "open_hyperlink", "select_command". Mouse bindings try this
// first, then fall back to parseAction for keyboard-callable actions.
std::optional<MouseAction::Any> parseMouseAction(const std::string& name,
                                                  const std::vector<std::string>& args);

// Convert a list of BindingConfigs (from TOML) into Bindings.
std::vector<Binding>        parseBindings(const std::vector<BindingConfig>& configs);

// Built-in default bindings (used when no config overrides them).
std::vector<Binding>        defaultBindings();

// --- Mouse bindings ---

struct MouseBindingConfig; // defined in Config.h

struct MouseStroke {
    MouseButton    button = MouseButton::Left;
    uint32_t       mods   = 0;
    MouseEventType event  = MouseEventType::Press;
    MouseMode      mode   = MouseMode::Ungrabbed;
    MouseRegion    region = MouseRegion::Any;

    bool matches(const MouseStroke& incoming) const noexcept;
};

// Result of a mouse-binding lookup: either a keyboard-style action (forwarded
// to PlatformDawn::dispatchAction) or a mouse-only action that needs cell
// context (handled inline in InputController). The split is intentional —
// mouse-only types can't usefully run from a keyboard context, and keyboard
// types can't carry click coordinates.
using MouseBindingResult = std::variant<Action::Any, MouseAction::Any>;

// Unified type-index key across both halves of MouseBindingResult, used by
// mergeMouseBindings dedup. Action::Any indices occupy [0, Action::count);
// MouseAction::Any indices are offset by Action::count to share one space.
inline std::size_t mouseBindingTypeKey(const MouseBindingResult& r) {
    if (auto* a = std::get_if<Action::Any>(&r)) return Action::typeOf(*a);
    return Action::count + MouseAction::typeOf(std::get<MouseAction::Any>(r));
}

struct MouseBinding {
    MouseStroke         trigger;
    MouseBindingResult  action;
};

std::vector<MouseBinding> parseMouseBindings(const std::vector<MouseBindingConfig>& configs);
std::vector<MouseBinding> defaultMouseBindings();
// Merge defaults with user bindings: any default whose action type is bound
// by at least one user binding is dropped, then user bindings are appended.
// Lets a user binding for e.g. ctrl+left → open_hyperlink suppress every
// default that maps to OpenHyperlink (regardless of stroke), so the user can
// fully take over an action without inheriting the built-in trigger.
std::vector<MouseBinding> mergeMouseBindings(std::vector<MouseBinding> defaults,
                                              std::vector<MouseBinding> user);
// Returns every action bound to this stroke, in declaration order. Callers
// run them in sequence; binding two actions to the same stroke is supported
// so e.g. Cmd+Click can both open a hyperlink and select the containing
// OSC 133 command.
std::vector<MouseBindingResult> matchMouseBindings(const MouseStroke& stroke,
                                                    const std::vector<MouseBinding>& bindings);

// Sequence state machine: accumulates keypresses and matches against a binding table.
class SequenceMatcher {
public:
    enum class Result { Match, Prefix, NoMatch };

    struct MatchResult {
        Result                     result;
        // Every binding whose key sequence equals the accumulated chord, in
        // declaration order. Empty for Prefix/NoMatch. Multiple entries are
        // supported — binding the same stroke twice fires both actions, same
        // model as matchMouseBindings.
        std::vector<Action::Any>   actions;
        // On NoMatch after one or more Prefix steps, holds the prefix
        // keys that were swallowed (all accumulated keys except the one
        // that caused the NoMatch). The caller can resend these to the
        // shell so prefix keystrokes aren't silently lost when the user
        // aborts a multi-key binding. Empty for Match/Prefix, and empty
        // for NoMatch at the root of the sequence.
        std::vector<KeyStroke>     abortedPrefix;
    };

    // Feed the next keystroke. Returns the match status.
    MatchResult advance(const KeyStroke& ks, const std::vector<Binding>& bindings);

    // Reset accumulated sequence (called automatically on Match and NoMatch).
    void reset();

private:
    std::vector<KeyStroke> current_;
};
