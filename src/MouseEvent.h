#ifndef MOUSEEVENT_H
#define MOUSEEVENT_H

#include "InputEvent.h"

class MouseEvent : public InputEvent
{
public:
    size_t x { 0 }, y { 0 }, windowX { 0 }, windowY { 0 };

    enum Button {
        NoButton = 0,
        Left = (1ull << 1),
        Mid = (1ull << 2),
        Right = (1ull << 3)
    } button { NoButton };

    unsigned int buttons { NoButton };
};


#endif /* MOUSEEVENT_H */
