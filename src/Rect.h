#pragma once

// Pixel rectangle in window coordinate space. Shared by the layout tree
// (computed slot rects) and the terminal rendering path (per-pane rects).
// Aliased as both `LayoutRect` and `PaneRect` so existing call sites keep
// working — the two names were kept distinct while the tree cutover was in
// progress; post-cutover they're the same type.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool isEmpty() const { return w <= 0 || h <= 0; }
    bool operator==(const Rect&) const = default;
};
