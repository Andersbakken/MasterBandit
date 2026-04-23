#pragma once

// Pixel rectangle in window coordinate space. Shared by the layout tree
// (computed slot rects) and the terminal rendering path (per-pane rects).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool isEmpty() const { return w <= 0 || h <= 0; }
    bool operator==(const Rect&) const = default;
};
