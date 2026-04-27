#pragma once

#include "DragHandler.h"

class TerminalEmulator;

// State carrier for an in-progress text selection drag (Normal or Rectangle
// mode). Created on the press that begins a selection; destroyed on release.
//
// Motion handling currently stays inline in InputController::onCursorPos
// (lines 869-898) — that code reads state via DragHandler getters from the
// activeDrag_ pointer instead of the old selectionDrag* bools. Once a second
// drag kind lands and forces extraction, this class will override
// DragHandler::onMotion / onRelease.
class SelectionDrag : public DragHandler {
public:
    SelectionDrag(double sx, double sy, MouseButton button, TerminalEmulator* term)
        : DragHandler(sx, sy, button), term_(term) {}

    TerminalEmulator* term() const { return term_; }

private:
    TerminalEmulator* term_;
};
