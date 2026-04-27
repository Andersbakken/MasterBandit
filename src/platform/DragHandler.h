#pragma once

#include "InputTypes.h"

#include <eventloop/Window.h>

// Base class for in-progress mouse drag gestures held by InputController.
//
// Today there is one subclass (SelectionDrag) and the motion / release logic
// for it still lives inline in InputController, reading state via the getters
// below. Future drag handlers (divider resize, pane move, tab reorder) will
// override `onMotion` / `onRelease` and run their own logic; once a second
// subclass exists, the selection-drag motion can be moved into
// SelectionDrag::onMotion as well.
//
// Lifetime: created by InputController on a button press that begins a drag,
// stored in InputController::activeDrag_, destroyed on the matching release
// (or when canceled). Subclasses may hold raw pointers to platform / terminal
// objects whose lifetime is guaranteed to outlive the drag (only the press
// path that constructs the drag controls when those become invalid).
class DragHandler {
public:
    virtual ~DragHandler() = default;

    // Called from onCursorPos while the drag is active. Default is a no-op
    // because today's only drag (selection) still has its motion handled
    // inline in InputController. Override for new drag kinds.
    virtual void onMotion(double /*sx*/, double /*sy*/) {}

    // Called when the originating button is released or the drag is canceled
    // (e.g. focus loss). Default is a no-op for the same reason as onMotion.
    virtual void onRelease(bool /*canceled*/) {}

    // Cursor shape to display while the drag is active. Defaults to Arrow;
    // selection drag stays IBeam, divider drag will return ResizeH/V, etc.
    virtual Window::CursorStyle cursorStyle() const { return Window::CursorStyle::Arrow; }

    // 3-pixel deadband: drags don't fire term-side motion until the cursor
    // has moved this far from the press point. Caller (InputController) flips
    // started_ to true the first time the deadband is exceeded.
    bool started() const { return started_; }
    void setStarted(bool s) { started_ = s; }

    double originX() const { return originX_; }
    double originY() const { return originY_; }

    MouseButton button() const { return button_; }

protected:
    DragHandler(double sx, double sy, MouseButton button)
        : originX_(sx), originY_(sy), button_(button) {}

private:
    double originX_;
    double originY_;
    MouseButton button_;
    bool started_ = false;
};
