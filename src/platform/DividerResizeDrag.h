#pragma once

#include "DragHandler.h"
#include "LayoutTree.h" // for SplitDir

#include <Uuid.h>
#include <eventloop/Window.h>

// State carrier for an in-progress divider-resize drag. Created when the user
// presses left-click on a `MouseRegion::Divider` cell; destroyed on release.
//
// `ownerPaneId` is the leftmost terminal Uuid in the first child of the
// container being resized (as reported by tabDividersWithOwnerPanes). It's
// what `Engine::resizeTabPaneEdge` walks up from to find the matching
// container; using the leaf as the anchor avoids stashing a container Uuid
// that could move under us during the drag.
//
// `axis` is the parent container's split direction: `SplitDir::Horizontal`
// for a vertical divider line between left/right panes (drag X resizes),
// `SplitDir::Vertical` for a horizontal line between top/bottom panes
// (drag Y resizes).
//
// `lastSx`/`lastSy` track the cursor's last sampled position so each motion
// event computes an incremental pixel delta along the axis. `resizeEdgeAlongAxis`
// applies the delta to the parent's child stretch hints, so accumulating
// per-event deltas yields a smooth drag without integral drift.
class DividerResizeDrag : public DragHandler
{
public:
    DividerResizeDrag(double sx, double sy, MouseButton button,
                      Uuid ownerPaneId, Uuid subtreeRoot, SplitDir axis)
        : DragHandler(sx, sy, button)
        , ownerPaneId_(ownerPaneId)
        , subtreeRoot_(subtreeRoot)
        , axis_(axis)
        , lastSx_(sx)
        , lastSy_(sy)
    {
    }

    Uuid ownerPaneId() const { return ownerPaneId_; }

    Uuid subtreeRoot() const { return subtreeRoot_; }

    SplitDir axis() const { return axis_; }

    double lastSx() const { return lastSx_; }

    double lastSy() const { return lastSy_; }

    void setLast(double sx, double sy)
    {
        lastSx_ = sx;
        lastSy_ = sy;
    }

    Window::CursorStyle cursorStyle() const override
    {
        return axis_ == SplitDir::Horizontal
            ? Window::CursorStyle::ResizeH
            : Window::CursorStyle::ResizeV;
    }

private:
    Uuid ownerPaneId_;
    Uuid subtreeRoot_;
    SplitDir axis_;
    double lastSx_;
    double lastSy_;
};
