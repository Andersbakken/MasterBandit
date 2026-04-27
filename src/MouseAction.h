#pragma once
#include "Utils.h"  // overloaded<> helper for std::visit (parity with Action.h)

#include <array>
#include <cstddef>
#include <string_view>
#include <variant>

// Mouse-only action types — counterpart to Action::Any. These need cell
// coordinates to do anything useful, so they aren't wired through the generic
// Action dispatcher (Platform_Actions.cpp); InputController dispatches them
// directly at the press/click site, where the click context is in scope.
//
// Mouse bindings produce Bindings::MouseBindingResult, which is a variant of
// (Action::Any, MouseAction::Any). The keyboard half routes through
// dispatchAction; the mouse half lands here.
namespace MouseAction {

enum class SelectionType { Normal, Word, Line, Extend, Rectangle };

// Begin a selection at the click. Normal/Rectangle install a selection drag
// handler in InputController; Word/Line/Extend run their term-side action
// immediately and don't drag. Renamed from Action::MouseSelection — the
// user-facing binding name in TOML stays "mouse_selection".
struct StartSelection { SelectionType type; };

// Open the OSC 8 hyperlink under the cursor (no-op if no URL is present).
struct OpenHyperlink {};

// Set the OSC 133 selected-command highlight to the command containing the
// click's row, or clear it if the row has no command.
struct SelectCommand {};

using Any = std::variant<StartSelection, OpenHyperlink, SelectCommand>;

using TypeIndex = std::size_t;

inline TypeIndex typeOf(const Any& a) { return a.index(); }

inline constexpr TypeIndex count = std::variant_size_v<Any>;

template<typename T>
constexpr TypeIndex indexOf() { return Any(T{}).index(); }

namespace detail {
template<typename T>
constexpr std::string_view rawTypeName() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#endif
}

template<typename T>
constexpr std::string_view typeName() {
    constexpr auto raw = rawTypeName<T>();
    constexpr auto teq = raw.find("T = ");
    static_assert(teq != std::string_view::npos);
    constexpr auto start = raw.find("::", teq + 4);
    static_assert(start != std::string_view::npos);
    constexpr auto nameStart = start + 2;
    constexpr auto end = raw.find_first_of("];", nameStart);
    static_assert(end != std::string_view::npos);
    return raw.substr(nameStart, end - nameStart);
}

template<typename... Ts>
constexpr auto makeNameTable(std::variant<Ts...>*) {
    return std::array<std::string_view, sizeof...(Ts)>{ typeName<Ts>()... };
}
} // namespace detail

inline constexpr auto nameTable = detail::makeNameTable(static_cast<Any*>(nullptr));

inline constexpr std::string_view nameOf(TypeIndex idx) {
    return idx < count ? nameTable[idx] : std::string_view{};
}

inline constexpr TypeIndex indexOfName(std::string_view name) {
    for (TypeIndex i = 0; i < count; ++i) {
        if (nameTable[i] == name) return i;
    }
    return count;
}

} // namespace MouseAction
