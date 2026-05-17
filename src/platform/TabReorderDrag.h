#pragma once

#include "DragHandler.h"

#include <Uuid.h>

class PlatformDawn;

// State carrier for an in-progress tab-bar drag-to-reorder gesture. Created
// on a left-button press that lands on a tab cell; destroyed on release or
// Esc-cancel.
//
// Motion handling lives inline in InputController::onCursorPos for the same
// reason SelectionDrag's does — hit-testing relies on InputController helpers
// (resolveTabBarClickIndex, the render-state colRanges) that aren't worth
// extracting onto a free interface for one consumer. Once the cursor has
// moved past `thresholdPx` horizontal pixels from the press point, InputController
// flips `started_` and calls `PlatformDawn::setDraggedTab(barId, draggedTabId)`
// so populateTabBars stamps the highlight onto the affected bar. Subsequent
// midpoint crossings call `LayoutTree::moveChild(stackId, draggedTabId, ±1)`.
//
// `dragStartIndex` is the position the tab held at press time, used by the
// Esc-cancel path in InputController to rewind via a single `moveChild`.
// `currentIndex` is the position the tab holds right now after all in-flight
// moves; updated by the InputController motion handler so the next midpoint
// computation knows where to look.
class TabReorderDrag : public DragHandler
{
public:
    TabReorderDrag(double sx, double sy, MouseButton button, PlatformDawn *platform,
                   Uuid barId, Uuid stackId, Uuid draggedTabId,
                   int dragStartIndex, int thresholdPx)
        : DragHandler(sx, sy, button)
        , platform_(platform)
        , barId_(barId)
        , stackId_(stackId)
        , draggedTabId_(draggedTabId)
        , dragStartIndex_(dragStartIndex)
        , currentIndex_(dragStartIndex)
        , thresholdPx_(thresholdPx)
    {
    }

    PlatformDawn *platform() const { return platform_; }

    Uuid barId() const { return barId_; }

    Uuid stackId() const { return stackId_; }

    Uuid draggedTabId() const { return draggedTabId_; }

    int dragStartIndex() const { return dragStartIndex_; }

    int currentIndex() const { return currentIndex_; }

    void setCurrentIndex(int idx) { currentIndex_ = idx; }

    int thresholdPx() const { return thresholdPx_; }

private:
    PlatformDawn *platform_;
    Uuid barId_;
    Uuid stackId_;
    Uuid draggedTabId_;
    int dragStartIndex_;
    int currentIndex_;
    int thresholdPx_;
};
