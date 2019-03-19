#ifndef INPUTEVENT_H
#define INPUTEVENT_H

class InputEvent
{
public:
    enum Type {
        NoEvent,
        KeyPress,
        KeyRelease,
        MousePress,
        MouseRelease,
        MouseMove
    } type { NoEvent };


    enum Modifier {
        NoModifier = 0,
        ControlModifier = (1ull << 1),
        AltModifier = (1ull << 2),
        ShiftModifier = (1ull << 3),
        MetaModifier = (1ull << 4)
    };

    unsigned int modifiers { NoModifier };
};


#endif /* INPUTEVENT_H */
