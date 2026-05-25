#pragma once
#include "Utils.h" // overloaded<> helper for std::visit
#include "Uuid.h"
#include "platform/InputTypes.h" // Key, modifier flags (SendKey)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Action {

// Spatial directions + cyclic navigation. Defined before the schema
// infrastructure because ArgKind::Direction translation helpers below
// reference this enum.
enum class Direction
{
    Left,
    Right,
    Up,
    Down,
    Next,
    Prev
};

// ---------------------------------------------------------------------------
// Typed action arguments (post-2026-05 rework)
//
// Every action declares an ActionSchema listing its arguments by name and
// kind. The schema is the single source of truth used by:
//   - the TOML / JS "positional adapter" that coerces string-only inputs
//     into typed values for back-compat with the old positional API;
//   - the JS object-arg path (mb.invokeAction("name", {a, b, c}));
//   - the command palette to render appropriate input UI per arg;
//   - the type-generation that backs MbActionMap in mb.d.ts (hand-written
//     today, but the schema is the closed source set if we ever generate).
//
// `ArgValue` is purpose-built. It is NOT a general JSON value — the type
// set is exactly what action arguments can be. Uuid is carried as a
// string (the canonical 36-char form); the ArgKind::Uuid declaration in
// the schema triggers parse-on-validate. There is no nested map shape
// because object-of-objects argument shapes have not come up.
// ---------------------------------------------------------------------------

class ArgValue;
using ArgValueList = std::vector<ArgValue>;

class ArgValue
{
public:
    using Storage = std::variant<
        std::monostate, // missing / null
        bool,
        int64_t,
        double,
        std::string, // also holds Uuid strings; ArgKind decides
        ArgValueList>;

    ArgValue() = default;

    ArgValue(bool v)
        : v_(v)
    {
    }

    ArgValue(int v)
        : v_(static_cast<int64_t>(v))
    {
    }

    ArgValue(int64_t v)
        : v_(v)
    {
    }

    ArgValue(double v)
        : v_(v)
    {
    }

    ArgValue(const char *v)
        : v_(std::string(v))
    {
    }

    ArgValue(std::string v)
        : v_(std::move(v))
    {
    }

    ArgValue(ArgValueList v)
        : v_(std::move(v))
    {
    }

    bool isNull() const { return std::holds_alternative<std::monostate>(v_); }

    template <typename T>
    bool is() const
    {
        return std::holds_alternative<T>(v_);
    }

    template <typename T>
    const T *get_if() const
    {
        return std::get_if<T>(&v_);
    }

    template <typename T>
    const T &get() const
    {
        return std::get<T>(v_);
    }

    const Storage &storage() const { return v_; }

private:
    Storage v_;
};

// Map keyed by ArgKind::name in the schema. Lookups for missing keys
// return ArgValue{} (null), so optional args naturally read as null.
using ArgsValue = std::unordered_map<std::string, ArgValue>;

// What kind of value an ActionArg accepts. The kind drives:
//   - palette UI (string -> input, enum -> list, bool -> two-button row)
//   - adapter coercion (the positional adapter applies kind-specific
//     parsing per arg: stoi for Int, fromString for Uuid, etc.)
//   - TS type emission (each kind maps to one of: string, number, boolean,
//     enum union, "uuid-string").
enum class ArgKind : uint8_t
{
    String,    // free-form string
    Int,       // signed 64-bit; bound to Int64 internally
    Bool,      // true / false
    Enum,      // value must be one of `enumValues` (static, case-sensitive)
    Direction, // syntactic sugar for Enum with the six direction tokens
    Uuid,      // 36-char canonical UUID string
};

// Source of dynamic value suggestions for a Provider-backed arg. Empty
// means the arg has no provider (palette free-text or static Enum).
//
// Lookup is in two registries: a built-in C++ registry seeded at startup
// (e.g. "panes" -> current pane titles), and a JS-side registry registered
// via mb.registerActionValueProvider. The "js:" prefix is reserved for
// the latter to keep the namespaces disjoint.
struct ValueOption
{
    std::string value; // canonical value sent to the action
    std::string label; // human-readable label for UI; defaults to value
};

struct ActionArg
{
    std::string name;
    std::string label; // human-readable; defaults to name when empty
    ArgKind kind  = ArgKind::String;
    bool required = false;
    // Default value for optional args. Used when:
    //  - palette user submits a form without touching the field
    //  - positional adapter sees a short args list
    // Null when no default (then optional args read as missing).
    ArgValue defaultValue;
    // Static enum values for ArgKind::Enum. ArgKind::Direction implicitly
    // populates this with the six tokens at schema-construction time, so
    // downstream code can treat Direction as a special-case Enum.
    std::vector<ValueOption> enumValues;
    // Named provider for dynamic value suggestions ("" if none).
    std::string valuesProvider;
};

struct ActionSchema
{
    std::vector<ActionArg> args;
};

// Look up the schema for a built-in C++ action by its Pascal name
// (Action::nameOf(idx)). Returns nullptr for actions without registered
// schemas; treat those as "no args declared".
const ActionSchema *schemaFor(std::string_view pascalName);

// Look up a value provider by name. Returns an empty list if the
// provider isn't registered, which the palette treats as "no
// suggestions; free-text prompt".
using ValueProviderFn = std::function<std::vector<ValueOption>()>;
void registerValueProvider(std::string name, ValueProviderFn fn);
std::vector<ValueOption> queryValueProvider(std::string_view name);

// Direction <-> string. The Direction ArgKind treats values as strings on
// the wire ("left", "right", "up", "down", "next", "prev"); these helpers
// bridge to the typed enum.
std::optional<Direction> parseDirection(std::string_view s);
std::string_view directionToString(Direction d);

// Positional-string back-compat adapter. Given an action's schema and a
// list of positional string args (TOML or JS variadic), coerce them into
// an ArgsValue. On success returns the typed args. On error returns
// nullopt and sets `errorOut` to a human-readable reason. Schemas whose
// args are typed-but-have-no-default-and-no-positional cause an error
// when required.
std::optional<ArgsValue> coercePositional(const ActionSchema &schema,
                                          const std::vector<std::string> &positional,
                                          std::string &errorOut);

// Read an ArgValue out of an ArgsValue, applying the field's kind to
// coerce a string-typed entry (TOML rarely sends bools/ints; everything
// is strings on the wire from there). Used both at dispatch validation
// time and by Bindings.cpp parseAction during the transitional period.
// Returns nullopt + errorOut on coercion failure.
std::optional<ArgValue> readArg(const ActionSchema &schema,
                                const ArgsValue &args,
                                const std::string &name,
                                std::string &errorOut);

struct NewTab
{
};

// target = direct child of the owning Stack (top-level tab or sub-stack
// child). target.isNil() + index >= 0 = top-level tab by index. Both nil/
// negative = the active top-level tab.
struct CloseTab
{
    Uuid target;
    int index = -1;
};

// Cycle the activeChild of a Stack by `delta` positions. `stack` is the
// Stack to cycle in: nil means "resolve at handler time from the focused
// pane's nearest enclosing Stack" (so a single keybinding cycles the
// containing sub-bar when focus is in one, else top-level tabs).
struct ActivateTabRelative
{
    Uuid stack;
    int delta = 0;
};

// target = direct child of the owning Stack. target.isNil() + index >= 0
// resolves to root stack child at that index.
struct ActivateTab
{
    Uuid target;
    int index = -1;
};

// dir = where the new pane appears (Right/Down/Left/Up)
struct SplitPane
{
    Direction dir;
};

struct ClosePane
{
};

struct ZoomPane
{
};

// Spatial (Left/Right/Up/Down) or cyclic (Next/Prev)
struct FocusPane
{
    Direction dir;
};

// Resize: grow current pane toward dir by `amount` cells
struct AdjustPaneSize
{
    Direction dir;
    int amount;
};

struct Copy
{
};

struct Paste
{
};

struct ScrollUp
{
    int lines = 3;
};

struct ScrollDown
{
    int lines = 3;
};

// Scroll the viewport by one "page" of the active terminal — resolved at
// dispatch time as (visible rows - 1), so a page-sized scroll always
// matches the current terminal geometry and keeps one row of overlap for
// reading context across the jump. Default-bound to Ctrl+PageUp/PageDown.
struct ScrollPageUp
{
};

struct ScrollPageDown
{
};

struct ScrollToTop
{
};

struct ScrollToBottom
{
};

struct IncreaseFontSize
{
};

struct DecreaseFontSize
{
};

struct ResetFontSize
{
};

struct ScrollToPrompt
{
    int direction = -1;
}; // -1 = previous, +1 = next

// Reorder actions. MoveTab swaps the active tab with its left/right neighbor
// in the root stack (delta ±1). SwapPane swaps the focused pane with the
// pane in the given direction — spatial Left/Right/Up/Down mirrors FocusPane
// (pixel-step past divider gap), Next/Prev wraps through the tab's leaves
// in tree-traversal order. RotatePanes circular-shifts every leaf in the
// active tab by `delta` positions, regardless of nested container shape;
// the active focus follows the same Uuid into its new visual position.
struct MoveTab
{
    int delta;
};

struct SwapPane
{
    Direction dir;
};

struct RotatePanes
{
    int delta;
};

struct SelectCommandOutput
{
};

struct CopyLastCommand
{
}; // prompt + input + output of last command to clipboard

struct CopySelectedCommandOutput
{
}; // output of currently selected OSC 133 command (no-op if none)

struct CopyDocument
{
}; // entire scrollback + screen as plain text to clipboard

struct FocusPopup
{
}; // Cycle focus: main terminal → popup1 → popup2 → ... → main

struct ReloadConfig
{
};

struct ScriptAction
{
    std::string name;
    std::vector<std::string> args;
};

// Mouse actions. PasteSelection is cell-independent (pastes the system
// primary/clipboard regardless of click position) so it stays here and
// remains keyboard- and IPC-callable. The cell-aware mouse types
// (StartSelection, OpenHyperlink, SelectCommand) live in MouseAction::Any
// and are dispatched by InputController where click context is in scope.
struct PasteSelection
{
};

// Clear the focused terminal. `mode` selects what to wipe:
//   Scrollback — drop history rows. Live grid (prompt, current input,
//                command output in progress) and cursor untouched.
//                Implemented via `\x1b[3J`, which no-ops on alt screen
//                (`TerminalEmulator.cpp:1941`).
//   All        — wipe the live grid AND scrollback, lifting the
//                in-progress prompt span (OSC 133 promptStart → cursor
//                row, or just the cursor row if OSC 133 absent) to the
//                top so the user sees the prompt at row 0 of an empty
//                screen. Whole action is a no-op on alt screen.
enum class ClearMode : uint8_t
{
    Scrollback,
    All,
};

struct Clear
{
    ClearMode mode = ClearMode::All;
};

// Write `text` to the focused pane's PTY as if it were typed. No bracketed-
// paste wrapping (use Paste for clipboard-style input). Useful for binding
// macros like `Cmd+K → send_string "clear\n"`.
struct SendString
{
    std::string text;
};

// Synthesize a keystroke and route it through the terminal's keyboard
// encoder (kitty / cursor-key / app-keypad mode all apply). Single keystroke
// per call — chain via Multiple if/when it lands.
struct SendKey
{
    Key key       = Key_unknown;
    uint32_t mods = 0;
};

// Bound key consumed, no action. Use to shadow a default binding without
// rebinding it to something else.
struct Nop
{
};

// Drops the matching default binding entirely. The stroke is NOT bound after
// merge — it falls through to normal input handling as if no binding existed.
// Handled in mergeKeyBindings; dispatch is a no-op (defensive).
struct DisableDefaultAssignment
{
};

// Activate the previously-active child of `stack`. Nil stack resolves to the
// root tabs Stack at dispatch time (same convention as ActivateTabRelative).
// Per-stack history is kept in the JS controller.
struct ActivateLastTab
{
    Uuid stack;
};

// Hard reset (RIS / ESC c) injected into the focused emulator's parser.
// No alt-screen guard — the parser's RIS handler decides what to wipe.
struct ResetTerminal
{
};

using Any = std::variant<
    NewTab, CloseTab, ActivateTabRelative, ActivateTab,
    SplitPane, ClosePane, ZoomPane, FocusPane, AdjustPaneSize,
    Copy, Paste,
    ScrollUp, ScrollDown, ScrollPageUp, ScrollPageDown, ScrollToTop, ScrollToBottom,
    IncreaseFontSize, DecreaseFontSize, ResetFontSize,
    ScrollToPrompt, SelectCommandOutput,
    CopyLastCommand, CopySelectedCommandOutput, CopyDocument,
    FocusPopup, ReloadConfig, ScriptAction,
    PasteSelection,
    MoveTab, SwapPane, RotatePanes,
    Clear,
    SendString, SendKey, Nop, DisableDefaultAssignment, ActivateLastTab, ResetTerminal>;

// Action type identity is the variant index.
using TypeIndex = std::size_t;

inline TypeIndex typeOf(const Any &a)
{
    return a.index();
}

// Compile-time index for a specific action struct: Action::indexOf<Action::NewTab>()
template <typename T>
constexpr TypeIndex indexOf()
{
    // Instantiate a variant holding T and return its index.
    return Any(T {}).index();
}

// Total number of action types.
inline constexpr TypeIndex count = std::variant_size_v<Any>;

// Compile-time name table — maps variant index to struct name.
namespace detail {
template <typename T>
constexpr std::string_view rawTypeName()
{
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#endif
}

// Extract the short struct name from __PRETTY_FUNCTION__ output.
// For Clang: "std::string_view Action::detail::rawTypeName() [T = Action::NewTab]"
template <typename T>
constexpr std::string_view typeName()
{
    constexpr auto raw = rawTypeName<T>();
    // Find "Action::" after "T = "
    constexpr auto teq = raw.find("T = ");
    static_assert(teq != std::string_view::npos);
    constexpr auto start = raw.find("::", teq + 4);
    static_assert(start != std::string_view::npos);
    constexpr auto nameStart = start + 2;
    // GCC appends "; alias = expanded_type" inside [with ...]; stop at ; or ]
    constexpr auto end       = raw.find_first_of("];", nameStart);
    static_assert(end != std::string_view::npos);
    return raw.substr(nameStart, end - nameStart);
}

template <typename... Ts>
constexpr auto makeNameTable(std::variant<Ts...> *)
{
    return std::array<std::string_view, sizeof...(Ts)> { typeName<Ts>()... };
}
} // namespace detail

// Action::nameOf(index) — returns e.g. "NewTab", "CloseTab"
inline constexpr auto nameTable = detail::makeNameTable(static_cast<Any *>(nullptr));

inline constexpr std::string_view nameOf(TypeIndex idx)
{
    return idx < count ? nameTable[idx] : std::string_view {};
}

// Action::indexOfName("NewTab") — returns the variant index, or count if not found
inline constexpr TypeIndex indexOfName(std::string_view name)
{
    for (TypeIndex i = 0; i < count; ++i) {
        if (nameTable[i] == name) {
            return i;
        }
    }
    return count;
}

// Build a typed Action::Any from a Pascal-cased action name and a
// typed-args map. Returns nullopt if:
//   - the name is not a registered built-in action, OR
//   - the schema validation fails (errorOut set to a human-readable
//     reason).
//
// Schema is the source of truth for argument coercion: ArgKind::Int
// strings get stoi-coerced, ArgKind::Direction strings get translated
// via parseDirection, etc. Each registered action declares (a) the
// schema describing wire shape, and (b) a builder callback that
// consumes the validated ArgsValue and produces the variant. Schema
// and builder are co-located in Action.cpp so they stay in lockstep.
//
// For "ScriptAction", call `buildScriptAction(name, args)` instead;
// the name carries the script-action namespace and is not a built-in
// Pascal identifier.
//
// Builder return value: optional<Any>. nullopt means the builder
// rejected the input even though the schema accepted it — e.g. an
// invalid keystroke string passed to SendKey, where the structural
// schema (ArgKind::String) cannot capture the semantic constraint.
// Builders that always succeed (the majority) just return `Any { … }`.
using ActionBuilder = std::function<std::optional<Any>(const ArgsValue &)>;

std::optional<Any> buildActionFromArgs(std::string_view pascalName,
                                       const ArgsValue &args,
                                       std::string &errorOut);

// Convenience: combine adapter + builder. Coerces positional strings
// using the schema, then builds the variant. Returns nullopt with
// errorOut set on any failure.
std::optional<Any> buildActionFromPositional(std::string_view pascalName,
                                             const std::vector<std::string> &positional,
                                             std::string &errorOut);

// Internal — registers a (name, schema, builder) triple. Called by
// Action.cpp's registerBuiltinSchemas().
void registerAction(std::string_view pascalName,
                    ActionSchema schema,
                    ActionBuilder builder);

// Trampoline registration: SendKey's schema-driven builder needs to
// parse keystroke strings ("ctrl+a", "f1") but the parser lives in
// Bindings.cpp. Bindings.cpp::parseKeyStroke depends on Action.h, so
// importing Bindings.cpp into Action.cpp would form a layering cycle.
// Bindings.cpp calls setSendKeyParser() once during static init,
// supplying the SendKey factory.
struct SendKey; // forward
void setSendKeyParser(std::function<SendKey(const std::string &)> fn);

// Listener called after an action is dispatched.
using Listener = std::function<void(TypeIndex, const Any &)>;

// Opaque handle returned by addListener, used to remove it.
using ListenerId = uint64_t;

class Dispatcher
{
public:
    // Listen to a specific action type. Returns a handle for removal.
    ListenerId addListener(TypeIndex type, Listener fn);
    // Listen to all action types.
    ListenerId addListener(Listener fn);
    // Remove a previously added listener.
    void removeListener(ListenerId id);
    // Notify all matching listeners. Called at the end of dispatchAction.
    void notify(TypeIndex type, const Any &action) const;

private:
    struct Entry
    {
        ListenerId id;
        TypeIndex type;
        bool allTypes;
        Listener fn;
    };

    std::vector<Entry> listeners_;
    ListenerId nextId_ = 1;
};

} // namespace Action
