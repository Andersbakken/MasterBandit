#include "Action.h"

#include <cctype>
#include <unordered_map>

namespace Action {

// ---------------------------------------------------------------------------
// Schema registry — populated in registerBuiltinSchemas() below, called
// once at static-init time. Built-in C++ actions live in a static map
// keyed by Pascal name (Action::nameOf(idx)).
// ---------------------------------------------------------------------------

namespace {

struct ActionRegistration
{
    ActionSchema schema;
    ActionBuilder builder;
};

std::unordered_map<std::string_view, ActionRegistration> &registry()
{
    // Function-local static avoids static-init-order issues. The map is
    // populated lazily on first access; see registerBuiltinSchemas.
    static std::unordered_map<std::string_view, ActionRegistration> m;
    return m;
}

std::unordered_map<std::string, ValueProviderFn> &providerMap()
{
    static std::unordered_map<std::string, ValueProviderFn> m;
    return m;
}

// Build the six-direction enum value list once. Direction is the most
// common ArgKind; instead of typing it out in every schema, ArgKind ==
// Direction implicitly carries this list. Stored once and copied into
// each arg's enumValues field at registration time.
const std::vector<ValueOption> &directionValueOptions()
{
    static const std::vector<ValueOption> v = {
        { "left", "Left" },
        { "right", "Right" },
        { "up", "Up" },
        { "down", "Down" },
        { "next", "Next" },
        { "prev", "Previous" },
    };
    return v;
}

// Coerce a single positional string into an ArgValue per arg kind.
// Returns nullopt + errorOut on parse failure.
std::optional<ArgValue> coerceOne(const ActionArg &arg,
                                  const std::string &raw,
                                  std::string &errorOut)
{
    switch (arg.kind) {
        case ArgKind::String:
            return ArgValue(raw);
        case ArgKind::Int: {
            try {
                size_t pos  = 0;
                long long v = std::stoll(raw, &pos);
                if (pos != raw.size()) {
                    errorOut = "arg '" + arg.name + "' is not an integer: " + raw;
                    return std::nullopt;
                }
                return ArgValue(static_cast<int64_t>(v));
            } catch (...) {
                errorOut = "arg '" + arg.name + "' is not an integer: " + raw;
                return std::nullopt;
            }
        }
        case ArgKind::Bool: {
            std::string lower = raw;
            for (auto &c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower == "true" || lower == "yes" || lower == "1") {
                return ArgValue(true);
            }
            if (lower == "false" || lower == "no" || lower == "0") {
                return ArgValue(false);
            }
            errorOut = "arg '" + arg.name + "' is not a boolean: " + raw;
            return std::nullopt;
        }
        case ArgKind::Enum:
        case ArgKind::Direction: {
            // Both flavors store the value as a string and validate
            // against enumValues. ArgKind::Direction had its enumValues
            // pre-populated at registration time.
            std::string lower = raw;
            for (auto &c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            for (const auto &opt : arg.enumValues) {
                if (opt.value == lower) {
                    return ArgValue(opt.value);
                }
            }
            errorOut = "arg '" + arg.name + "' is not one of the allowed values: " + raw;
            return std::nullopt;
        }
        case ArgKind::Uuid: {
            // Validation is best-effort: parseUuid + roundtrip would
            // pull in Uuid.cpp into this TU. The schema-driven dispatcher
            // will call Uuid::fromString anyway; carry the raw string
            // and let the dispatcher reject if it's malformed.
            return ArgValue(raw);
        }
    }
    errorOut = "arg '" + arg.name + "' has unknown ArgKind";
    return std::nullopt;
}

} // namespace

const ActionSchema *schemaFor(std::string_view pascalName)
{
    const auto &m = registry();
    auto it       = m.find(pascalName);
    return it == m.end() ? nullptr : &it->second.schema;
}

void registerAction(std::string_view pascalName, ActionSchema schema, ActionBuilder builder)
{
    registry()[pascalName] = ActionRegistration { std::move(schema), std::move(builder) };
}

std::optional<Any> buildActionFromArgs(std::string_view pascalName,
                                       const ArgsValue &args,
                                       std::string &errorOut)
{
    const auto &m = registry();
    auto it       = m.find(pascalName);
    if (it == m.end()) {
        errorOut = "unknown action '";
        errorOut.append(pascalName);
        errorOut += "'";
        return std::nullopt;
    }
    if (!it->second.builder) {
        errorOut = "action '";
        errorOut.append(pascalName);
        errorOut += "' has no builder";
        return std::nullopt;
    }
    // Pre-validate required args / fill in defaults. The builder
    // assumes the args map is normalized: every required field is
    // present, optionals either have their default applied or are
    // explicitly null.
    ArgsValue normalized;
    for (const auto &spec : it->second.schema.args) {
        auto found = args.find(spec.name);
        if (found != args.end() && !found->second.isNull()) {
            // Use readArg to apply ArgKind coercion (e.g. ArgKind::Int
            // on a string-typed entry).
            auto coerced = readArg(it->second.schema, args, spec.name, errorOut);
            if (!coerced) {
                return std::nullopt;
            }
            normalized[spec.name] = std::move(*coerced);
        } else if (!spec.defaultValue.isNull()) {
            normalized[spec.name] = spec.defaultValue;
        } else if (spec.required) {
            errorOut = "missing required arg '" + spec.name + "' for action '";
            errorOut.append(pascalName);
            errorOut += "'";
            return std::nullopt;
        }
        // else: optional + no default = absent, builder reads null.
    }
    auto result = it->second.builder(normalized);
    if (!result) {
        // Builder-level rejection (semantic, not structural). Fill in a
        // generic error if the builder didn't provide one — most
        // callers want to know which action failed.
        if (errorOut.empty()) {
            errorOut = "action '";
            errorOut.append(pascalName);
            errorOut += "' rejected its arguments";
        }
    }
    return result;
}

std::optional<Any> buildActionFromPositional(std::string_view pascalName,
                                             const std::vector<std::string> &positional,
                                             std::string &errorOut)
{
    const auto &m = registry();
    auto it       = m.find(pascalName);
    if (it == m.end()) {
        errorOut = "unknown action '";
        errorOut.append(pascalName);
        errorOut += "'";
        return std::nullopt;
    }
    auto coerced = coercePositional(it->second.schema, positional, errorOut);
    if (!coerced) {
        return std::nullopt;
    }
    return buildActionFromArgs(pascalName, *coerced, errorOut);
}

void registerValueProvider(std::string name, ValueProviderFn fn)
{
    providerMap()[std::move(name)] = std::move(fn);
}

std::vector<ValueOption> queryValueProvider(std::string_view name)
{
    auto it = providerMap().find(std::string(name));
    if (it == providerMap().end() || !it->second) {
        return {};
    }
    return it->second();
}

std::optional<Direction> parseDirection(std::string_view s)
{
    if (s == "left") {
        return Direction::Left;
    }
    if (s == "right") {
        return Direction::Right;
    }
    if (s == "up") {
        return Direction::Up;
    }
    if (s == "down") {
        return Direction::Down;
    }
    if (s == "next") {
        return Direction::Next;
    }
    if (s == "prev") {
        return Direction::Prev;
    }
    if (s == "previous") {
        return Direction::Prev;
    }
    if (s == "cw") {
        return Direction::Next; // rotate_panes alias
    }
    if (s == "ccw") {
        return Direction::Prev;
    }
    if (s == "clockwise") {
        return Direction::Next;
    }
    if (s == "counterclockwise") {
        return Direction::Prev;
    }
    return std::nullopt;
}

std::string_view directionToString(Direction d)
{
    switch (d) {
        case Direction::Left: return "left";
        case Direction::Right: return "right";
        case Direction::Up: return "up";
        case Direction::Down: return "down";
        case Direction::Next: return "next";
        case Direction::Prev: return "prev";
    }
    return "";
}

std::optional<ArgsValue> coercePositional(const ActionSchema &schema,
                                          const std::vector<std::string> &positional,
                                          std::string &errorOut)
{
    ArgsValue out;
    size_t idx = 0;
    for (const auto &arg : schema.args) {
        if (idx < positional.size()) {
            auto v = coerceOne(arg, positional[idx], errorOut);
            if (!v) {
                return std::nullopt;
            }
            out[arg.name] = std::move(*v);
            ++idx;
        } else if (!arg.defaultValue.isNull()) {
            out[arg.name] = arg.defaultValue;
        } else if (arg.required) {
            errorOut = "missing required arg '" + arg.name + "'";
            return std::nullopt;
        }
    }
    // Extra positional args past the declared schema are ignored
    // silently — they're a TOML / JS-variadic anachronism, not worth
    // refusing dispatch over.
    return out;
}

std::optional<ArgValue> readArg(const ActionSchema &schema,
                                const ArgsValue &args,
                                const std::string &name,
                                std::string &errorOut)
{
    const ActionArg *spec = nullptr;
    for (const auto &a : schema.args) {
        if (a.name == name) {
            spec = &a;
            break;
        }
    }
    if (!spec) {
        errorOut = "no such arg in schema: '" + name + "'";
        return std::nullopt;
    }
    auto it = args.find(name);
    if (it == args.end() || it->second.isNull()) {
        if (!spec->defaultValue.isNull()) {
            return spec->defaultValue;
        }
        if (spec->required) {
            errorOut = "missing required arg '" + name + "'";
            return std::nullopt;
        }
        return ArgValue {};
    }
    // If the args entry is already a typed non-string value, accept it
    // (came from the JS object-arg path). If it's a string, run it
    // through coerceOne so the wire-format ("3") becomes int64_t(3) for
    // ArgKind::Int, etc.
    if (auto *s = it->second.get_if<std::string>()) {
        return coerceOne(*spec, *s, errorOut);
    }
    return it->second;
}

// Helper used by the schema definitions below to build a Direction-kind
// arg in one line, with the six direction tokens pre-populated.
static ActionArg directionArg(std::string name, bool required, std::optional<Direction> defaultDir)
{
    ActionArg a;
    a.name       = std::move(name);
    a.kind       = ArgKind::Direction;
    a.required   = required;
    a.enumValues = directionValueOptions();
    if (defaultDir) {
        a.defaultValue = ArgValue(std::string(directionToString(*defaultDir)));
    }
    return a;
}

static ActionArg enumArg(std::string name, std::vector<ValueOption> values,
                         bool required, std::optional<std::string> defaultValue)
{
    ActionArg a;
    a.name       = std::move(name);
    a.kind       = ArgKind::Enum;
    a.required   = required;
    a.enumValues = std::move(values);
    if (defaultValue) {
        a.defaultValue = ArgValue(*defaultValue);
    }
    return a;
}

static ActionArg intArg(std::string name, bool required,
                        std::optional<int64_t> defaultValue,
                        std::optional<std::string> providerName = std::nullopt)
{
    ActionArg a;
    a.name     = std::move(name);
    a.kind     = ArgKind::Int;
    a.required = required;
    if (defaultValue) {
        a.defaultValue = ArgValue(*defaultValue);
    }
    if (providerName) {
        a.valuesProvider = *providerName;
    }
    return a;
}

static ActionArg boolArg(std::string name, bool required,
                         std::optional<bool> defaultValue)
{
    ActionArg a;
    a.name     = std::move(name);
    a.kind     = ArgKind::Bool;
    a.required = required;
    if (defaultValue) {
        a.defaultValue = ArgValue(*defaultValue);
    }
    return a;
}

static ActionArg stringArg(std::string name, bool required,
                           std::optional<std::string> defaultValue,
                           std::optional<std::string> providerName = std::nullopt)
{
    ActionArg a;
    a.name     = std::move(name);
    a.kind     = ArgKind::String;
    a.required = required;
    if (defaultValue) {
        a.defaultValue = ArgValue(*defaultValue);
    }
    if (providerName) {
        a.valuesProvider = *providerName;
    }
    return a;
}

static ActionArg uuidArg(std::string name, bool required,
                         std::optional<std::string> providerName = std::nullopt)
{
    ActionArg a;
    a.name     = std::move(name);
    a.kind     = ArgKind::Uuid;
    a.required = required;
    if (providerName) {
        a.valuesProvider = *providerName;
    }
    return a;
}

// Register every built-in C++ action's schema. Triggered once via a
// static instance below. Pascal names are used as map keys to match
// Action::nameOf(idx). Snake-case lookups (TOML "close_tab", JS
// invokeAction("close_tab")) are translated to Pascal via the existing
// snakeToPascal helper in Bindings.cpp at dispatch time.
//
// Schemas only declare args that are user-visible / configurable. They
// do NOT declare every field of the Action struct — for example
// CloseTab carries both a Uuid and an int, but the user supplies one or
// the other, and parseAction disambiguates by trying stoi first. The
// schema represents that as a single optional string arg whose
// dispatcher distinguishes uuid-vs-int. (Concrete: see "close_tab"
// below — single "target" arg of kind String, with documentation that
// it accepts either an index or a UUID.)
// Helpers used by builders below to extract typed values from an
// ArgsValue. Schema pre-validation ran in buildActionFromArgs, so a
// missing-required arg can't reach here — but optional args may be
// null. Read with sensible defaults at the builder boundary.
static std::string strOr(const ArgsValue &a, const std::string &k, std::string fallback = {})
{
    auto it = a.find(k);
    if (it == a.end()) {
        return fallback;
    }
    if (auto *s = it->second.get_if<std::string>()) {
        return *s;
    }
    return fallback;
}

static bool boolOr(const ArgsValue &a, const std::string &k, bool fallback = false)
{
    auto it = a.find(k);
    if (it == a.end()) {
        return fallback;
    }
    if (auto *v = it->second.get_if<bool>()) {
        return *v;
    }
    return fallback;
}

static int64_t intOr(const ArgsValue &a, const std::string &k, int64_t fallback = 0)
{
    auto it = a.find(k);
    if (it == a.end()) {
        return fallback;
    }
    if (auto *v = it->second.get_if<int64_t>()) {
        return *v;
    }
    return fallback;
}

static Direction dirOr(const ArgsValue &a, const std::string &k, Direction fallback = Direction::Right)
{
    auto s = strOr(a, k);
    return parseDirection(s).value_or(fallback);
}

// "target" args on ActivateTab / CloseTab accept either an integer
// index or a UUID. Disambiguates the same way the old parseAction did:
// try stoi first, then Uuid::fromString. Returns {Uuid, int}: empty
// Uuid + non-negative int = index; non-empty Uuid + -1 = UUID.
struct TabTarget
{
    Uuid uuid;
    int index = -1;
};

static TabTarget targetOr(const ArgsValue &a, const std::string &k)
{
    auto s = strOr(a, k);
    TabTarget t;
    if (s.empty()) {
        return t;
    }
    try {
        size_t pos = 0;
        int v      = std::stoi(s, &pos);
        if (pos == s.size()) {
            t.index = v;
            return t;
        }
    } catch (...) {
        // fall through to UUID path
    }
    t.uuid = Uuid::fromString(s);
    return t;
}

// Trampoline set by Bindings.cpp at module init so the SendKey builder
// can parse keystroke strings without pulling parseKeyStroke into this
// TU (which would form a layering cycle: Action.cpp → Bindings.cpp →
// Action.h).
//
// Function-local static (Meyer's singleton) avoids the static
// initialization order fiasco: when Bindings.cpp's namespace-level
// SendKeyParserInit constructor runs and calls setSendKeyParser, the
// translation-unit-level definition of g_sendKeyParser may or may not
// have been zero/value-initialized yet — depending on link order.
// First-use initialization makes the order well-defined: the first
// call to sendKeyParser() initializes the std::function to empty, and
// any subsequent setter sticks.
static std::function<SendKey(const std::string &)> &sendKeyParser()
{
    static std::function<SendKey(const std::string &)> fn;
    return fn;
}

void setSendKeyParser(std::function<SendKey(const std::string &)> fn)
{
    sendKeyParser() = std::move(fn);
}

static void registerBuiltinSchemas()
{
    registerAction("NewTab", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { NewTab {} };
                   });

    registerAction("CloseTab",
                   ActionSchema { { stringArg("target", false, std::nullopt, "tabs") } },
                   [](const ArgsValue &a)
                   {
                       auto t = targetOr(a, "target");
                       return Any { CloseTab { t.uuid, t.index } };
                   });

    registerAction("ActivateTabRelative",
                   ActionSchema {
                       { intArg("delta", true, std::nullopt),
                         uuidArg("stack", false),
                         boolArg("wrap", false, false) },
                   },
                   [](const ArgsValue &a)
                   {
                       auto stackStr = strOr(a, "stack");
                       Uuid stack    = stackStr.empty() ? Uuid {} : Uuid::fromString(stackStr);
                       return Any { ActivateTabRelative { stack,
                                                          static_cast<int>(intOr(a, "delta")),
                                                          boolOr(a, "wrap") } };
                   });

    registerAction("ActivateTab",
                   ActionSchema { { stringArg("target", true, std::nullopt, "tabs") } },
                   [](const ArgsValue &a)
                   {
                       auto t = targetOr(a, "target");
                       return Any { ActivateTab { t.uuid, t.index } };
                   });

    registerAction("SplitPane",
                   ActionSchema { { directionArg("direction", true, std::nullopt) } },
                   [](const ArgsValue &a)
                   {
                       return Any { SplitPane { dirOr(a, "direction") } };
                   });

    registerAction("ClosePane", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ClosePane {} };
                   });

    registerAction("ZoomPane", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ZoomPane {} };
                   });

    registerAction("FocusPane",
                   ActionSchema { { directionArg("direction", true, std::nullopt) } },
                   [](const ArgsValue &a)
                   {
                       return Any { FocusPane { dirOr(a, "direction") } };
                   });

    registerAction("AdjustPaneSize",
                   ActionSchema {
                       { directionArg("direction", true, std::nullopt),
                         intArg("amount", false, int64_t { 1 }) },
                   },
                   [](const ArgsValue &a)
                   {
                       return Any { AdjustPaneSize { dirOr(a, "direction"),
                                                     static_cast<int>(intOr(a, "amount", 1)) } };
                   });

    registerAction("Copy", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { Copy {} };
                   });
    registerAction("Paste", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { Paste {} };
                   });

    registerAction("ScrollUp",
                   ActionSchema { { intArg("lines", false, int64_t { 3 }) } },
                   [](const ArgsValue &a)
                   {
                       return Any { ScrollUp { static_cast<int>(intOr(a, "lines", 3)) } };
                   });
    registerAction("ScrollDown",
                   ActionSchema { { intArg("lines", false, int64_t { 3 }) } },
                   [](const ArgsValue &a)
                   {
                       return Any { ScrollDown { static_cast<int>(intOr(a, "lines", 3)) } };
                   });
    registerAction("ScrollPageUp", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ScrollPageUp {} };
                   });
    registerAction("ScrollPageDown", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ScrollPageDown {} };
                   });
    registerAction("ScrollToTop", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ScrollToTop {} };
                   });
    registerAction("ScrollToBottom", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ScrollToBottom {} };
                   });
    registerAction("IncreaseFontSize", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { IncreaseFontSize {} };
                   });
    registerAction("DecreaseFontSize", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { DecreaseFontSize {} };
                   });
    registerAction("ResetFontSize", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ResetFontSize {} };
                   });

    registerAction("ScrollToPrompt",
                   ActionSchema {
                       { enumArg("direction",
                                 { { "prev", "Previous" }, { "next", "Next" } },
                                 true,
                                 std::nullopt) },
                   },
                   [](const ArgsValue &a)
                   {
                       // ScrollToPrompt uses ±1 ints internally rather than enum.
                       return Any { ScrollToPrompt { strOr(a, "direction") == "next" ? 1 : -1 } };
                   });

    registerAction("SelectCommandOutput", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { SelectCommandOutput {} };
                   });
    registerAction("CopyLastCommand", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { CopyLastCommand {} };
                   });
    registerAction("CopySelectedCommandOutput", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { CopySelectedCommandOutput {} };
                   });
    registerAction("CopyDocument", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { CopyDocument {} };
                   });
    registerAction("FocusPopup", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { FocusPopup {} };
                   });
    registerAction("ReloadConfig", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ReloadConfig {} };
                   });
    registerAction("PasteSelection", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { PasteSelection {} };
                   });

    registerAction("MoveTab",
                   ActionSchema {
                       { enumArg("direction",
                                 { { "left", "Left" }, { "right", "Right" } },
                                 false,
                                 std::string { "right" }) },
                   },
                   [](const ArgsValue &a)
                   {
                       return Any { MoveTab { strOr(a, "direction") == "left" ? -1 : +1 } };
                   });

    registerAction("SwapPane",
                   ActionSchema { { directionArg("direction", false, Direction::Next) } },
                   [](const ArgsValue &a)
                   {
                       return Any { SwapPane { dirOr(a, "direction", Direction::Next) } };
                   });

    registerAction("RotatePanes",
                   ActionSchema {
                       { enumArg("direction",
                                 { { "cw", "Clockwise" }, { "ccw", "Counterclockwise" } },
                                 false,
                                 std::string { "cw" }) },
                   },
                   [](const ArgsValue &a)
                   {
                       return Any { RotatePanes { strOr(a, "direction") == "ccw" ? -1 : +1 } };
                   });

    registerAction("Clear",
                   ActionSchema {
                       { enumArg("mode",
                                 { { "all", "All" }, { "scrollback", "Scrollback" } },
                                 false,
                                 std::string { "all" }) },
                   },
                   [](const ArgsValue &a)
                   {
                       return Any { Clear { strOr(a, "mode") == "scrollback" ? ClearMode::Scrollback : ClearMode::All } };
                   });

    registerAction("SendString",
                   ActionSchema { { stringArg("text", true, std::nullopt) } },
                   [](const ArgsValue &a)
                   {
                       return Any { SendString { strOr(a, "text") } };
                   });

    // SendKey takes a keystroke string ("ctrl+a", "f1", etc.) and
    // parses it inline. We cannot call parseKeyStroke here (lives in
    // Bindings.cpp), so we just stash the text in a SendString-like
    // adapter? No — SendKey holds a Key+mods, the parsing has to
    // happen. Trampoline through Bindings.cpp via a callback set at
    // module init. Until that's wired, treat malformed input as
    // Key_unknown (the existing dispatch handler already no-ops on
    // Key_unknown).
    registerAction("SendKey",
                   ActionSchema { { stringArg("key", true, std::nullopt) } },
                   [](const ArgsValue &a) -> std::optional<Any>
                   {
                       // Resolved by the keystroke-parsing trampoline; see
                       // setSendKeyParser() called from Bindings.cpp init. Access
                       // through the Meyer's-singleton accessor so the function
                       // pointer survives the static-initialization-order fiasco.
                       auto &parser = sendKeyParser();
                       if (!parser) {
                           return std::nullopt;
                       }
                       auto sk = parser(strOr(a, "key"));
                       // The parser signals "unparseable keystroke" by returning
                       // Key_unknown. Reject the action so callers see nullopt
                       // (matches the old parseAction contract that returned
                       // nullopt for `parseKeyStroke` failures).
                       if (sk.key == Key_unknown) {
                           return std::nullopt;
                       }
                       return Any { sk };
                   });

    registerAction("Nop", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { Nop {} };
                   });
    registerAction("DisableDefaultAssignment", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { DisableDefaultAssignment {} };
                   });

    registerAction("ActivateLastTab",
                   ActionSchema { { uuidArg("stack", false) } },
                   [](const ArgsValue &a)
                   {
                       auto s = strOr(a, "stack");
                       return Any { ActivateLastTab { s.empty() ? Uuid {} : Uuid::fromString(s) } };
                   });

    registerAction("ResetTerminal", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ResetTerminal {} };
                   });

    // ScriptAction's own args are forwarded to the registered handler
    // and validated against that script's schema, if any. The outer
    // ScriptAction schema is empty: the variant carries (name, args)
    // verbatim through dispatch and never goes through this builder.
    registerAction("ScriptAction", ActionSchema { {} }, [](const ArgsValue &)
                   {
                       return Any { ScriptAction { {}, {} } };
                   });
}

namespace {
struct SchemaInit
{
    SchemaInit() { registerBuiltinSchemas(); }
} schemaInit;
} // namespace

ListenerId Dispatcher::addListener(TypeIndex type, Listener fn)
{
    ListenerId id = nextId_++;
    listeners_.push_back({ id, type, false, std::move(fn) });
    return id;
}

ListenerId Dispatcher::addListener(Listener fn)
{
    ListenerId id = nextId_++;
    listeners_.push_back({ id, {}, true, std::move(fn) });
    return id;
}

void Dispatcher::removeListener(ListenerId id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->id == id) {
            listeners_.erase(it);
            return;
        }
    }
}

void Dispatcher::notify(TypeIndex type, const Any &action) const
{
    for (const auto &entry : listeners_) {
        if (entry.allTypes || entry.type == type) {
            entry.fn(type, action);
        }
    }
}

} // namespace Action
