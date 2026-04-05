#pragma once
#include <variant>

// Overloaded helper for std::visit
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

namespace Action {

// Spatial directions + cyclic navigation
enum class Direction { Left, Right, Up, Down, Next, Prev };

struct NewTab             {};
struct CloseTab           {};
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

using Any = std::variant<
    NewTab, CloseTab, ActivateTabRelative, ActivateTab,
    SplitPane, ClosePane, ZoomPane, FocusPane, AdjustPaneSize,
    Copy, Paste,
    ScrollUp, ScrollDown, ScrollToTop, ScrollToBottom,
    PushOverlay, PopOverlay,
    IncreaseFontSize, DecreaseFontSize, ResetFontSize,
    ScrollToPrompt, SelectCommandOutput, ShowScrollback
>;

} // namespace Action
