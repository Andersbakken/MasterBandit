#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <variant>
#include <vector>

// Overloaded helper for std::visit
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

namespace Action {

// Spatial directions + cyclic navigation
enum class Direction { Left, Right, Up, Down, Next, Prev };

struct NewTab             {};
struct CloseTab           { int index = -1; }; // -1 = active tab
struct ActivateTabRelative { int delta; };
struct ActivateTab         { int index; };
// dir = where the new pane appears (Right/Down/Left/Up)
struct SplitPane           { Direction dir; };
struct ClosePane           {};
struct ZoomPane            {};
// Spatial (Left/Right/Up/Down) or cyclic (Next/Prev)
struct FocusPane           { Direction dir; };
// Resize: grow current pane toward dir by `amount` cells
struct AdjustPaneSize      { Direction dir; int amount; };
struct Copy                {};
struct Paste               {};
struct ScrollUp            { int lines = 3; };
struct ScrollDown          { int lines = 3; };
struct ScrollToTop         {};
struct ScrollToBottom      {};
struct PushOverlay         {};
struct PopOverlay          {};
struct IncreaseFontSize    {};
struct DecreaseFontSize    {};
struct ResetFontSize       {};
struct ScrollToPrompt      { int direction = -1; }; // -1 = previous, +1 = next
struct SelectCommandOutput {};
struct ShowScrollback      {};
struct FocusPopup          {}; // Cycle focus: main terminal → popup1 → popup2 → ... → main

using Any = std::variant<
    NewTab, CloseTab, ActivateTabRelative, ActivateTab,
    SplitPane, ClosePane, ZoomPane, FocusPane, AdjustPaneSize,
    Copy, Paste,
    ScrollUp, ScrollDown, ScrollToTop, ScrollToBottom,
    PushOverlay, PopOverlay,
    IncreaseFontSize, DecreaseFontSize, ResetFontSize,
    ScrollToPrompt, SelectCommandOutput, ShowScrollback,
    FocusPopup
>;

// Action type identity is the variant index.
using TypeIndex = std::size_t;

inline TypeIndex typeOf(const Any& a) { return a.index(); }

// Compile-time index for a specific action struct: Action::indexOf<Action::NewTab>()
template<typename T>
constexpr TypeIndex indexOf() {
    // Instantiate a variant holding T and return its index.
    return Any(T{}).index();
}

// Total number of action types.
inline constexpr TypeIndex count = std::variant_size_v<Any>;

// Compile-time name table — maps variant index to struct name.
namespace detail {
template<typename T>
constexpr std::string_view rawTypeName() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#endif
}

// Extract the short struct name from __PRETTY_FUNCTION__ output.
// For Clang: "std::string_view Action::detail::rawTypeName() [T = Action::NewTab]"
template<typename T>
constexpr std::string_view typeName() {
    constexpr auto raw = rawTypeName<T>();
    // Find "Action::" after "T = "
    constexpr auto teq = raw.find("T = ");
    static_assert(teq != std::string_view::npos);
    constexpr auto start = raw.find("::", teq + 4);
    static_assert(start != std::string_view::npos);
    constexpr auto nameStart = start + 2;
    constexpr auto end = raw.find(']', nameStart);
    static_assert(end != std::string_view::npos);
    return raw.substr(nameStart, end - nameStart);
}

template<typename... Ts>
constexpr auto makeNameTable(std::variant<Ts...>*) {
    return std::array<std::string_view, sizeof...(Ts)>{ typeName<Ts>()... };
}
} // namespace detail

// Action::nameOf(index) — returns e.g. "NewTab", "CloseTab"
inline constexpr auto nameTable = detail::makeNameTable(static_cast<Any*>(nullptr));

inline constexpr std::string_view nameOf(TypeIndex idx) {
    return idx < count ? nameTable[idx] : std::string_view{};
}

// Action::indexOfName("NewTab") — returns the variant index, or count if not found
inline constexpr TypeIndex indexOfName(std::string_view name) {
    for (TypeIndex i = 0; i < count; ++i) {
        if (nameTable[i] == name) return i;
    }
    return count;
}

// Listener called after an action is dispatched.
using Listener = std::function<void(TypeIndex, const Any&)>;

// Opaque handle returned by addListener, used to remove it.
using ListenerId = uint64_t;

class Dispatcher {
public:
    // Listen to a specific action type. Returns a handle for removal.
    ListenerId addListener(TypeIndex type, Listener fn);
    // Listen to all action types.
    ListenerId addListener(Listener fn);
    // Remove a previously added listener.
    void removeListener(ListenerId id);
    // Notify all matching listeners. Called at the end of dispatchAction.
    void notify(TypeIndex type, const Any& action) const;

private:
    struct Entry {
        ListenerId id;
        TypeIndex type;
        bool allTypes;
        Listener fn;
    };
    std::vector<Entry> listeners_;
    ListenerId nextId_ = 1;
};

} // namespace Action
