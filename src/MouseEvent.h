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

    static const char *buttonName(Button button)
    {
        switch (button) {
        case NoButton: return "none";
        case Left: return "left";
        case Right: return "right";
        case Mid: return "mid";
        }
    }

    static std::string buttonsName(unsigned int buttons)
    {
        std::string ret;
        for (Button button : { Left, Mid, Right }) {
            if (buttons & button) {
                if (!ret.empty())
                    ret += " + ";
                ret += buttonName(button);
            }
        }
        return ret;
    }

    unsigned int buttons { NoButton };
};


#endif /* MOUSEEVENT_H */
