#pragma once
#include "Action.h"
#include "Platform.h"
#include <optional>
#include <string>
#include <vector>

struct BindingConfig; // defined in Config.h

struct KeyStroke {
    Key          key  = Key_unknown;
    unsigned int mods = 0;

    bool operator==(const KeyStroke& o) const noexcept
    {
        return key == o.key && mods == o.mods;
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

// Convert a list of BindingConfigs (from TOML) into Bindings.
std::vector<Binding>        parseBindings(const std::vector<BindingConfig>& configs);

// Built-in default bindings (used when no config overrides them).
std::vector<Binding>        defaultBindings();

// Sequence state machine: accumulates keypresses and matches against a binding table.
class SequenceMatcher {
public:
    enum class Result { Match, Prefix, NoMatch };

    struct MatchResult {
        Result                    result;
        std::optional<Action::Any> action; // populated on Match
    };

    // Feed the next keystroke. Returns the match status.
    MatchResult advance(const KeyStroke& ks, const std::vector<Binding>& bindings);

    // Reset accumulated sequence (called automatically on Match and NoMatch).
    void reset();

private:
    std::vector<KeyStroke> current_;
};
