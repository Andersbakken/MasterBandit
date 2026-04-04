#pragma once
#include <variant>

// Overloaded helper for std::visit
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

namespace Action {

enum class Direction { Left, Right, Up, Down };
enum class SplitDir  { Horizontal, Vertical };

struct NewTab             {};
struct CloseTab           {};
struct ActivateTabRelative { int delta; };
struct ActivateTab         { int index; };
struct SplitPane           { SplitDir dir; };
struct ClosePane           {};
struct ZoomPane            {};
struct FocusPane           { Direction dir; };
struct Copy                {};
struct Paste               {};
struct ScrollUp            { int lines = 3; };
struct ScrollDown          { int lines = 3; };
struct ScrollToTop         {};
struct ScrollToBottom      {};
struct PushOverlay         {};
struct PopOverlay          {};

using Any = std::variant<
    NewTab, CloseTab, ActivateTabRelative, ActivateTab,
    SplitPane, ClosePane, ZoomPane, FocusPane,
    Copy, Paste,
    ScrollUp, ScrollDown, ScrollToTop, ScrollToBottom,
    PushOverlay, PopOverlay
>;

} // namespace Action
