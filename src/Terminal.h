#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>
#include "KeyEvent.h"
#include "MouseEvent.h"

class Window;
class Terminal
{
public:
    Terminal();
    ~Terminal();

    enum Flag {
        None = 0,
        LineWrap = (1ull << 1)
    };

    size_t x() const { return mX; }
    size_t y() const { return mY; }
    size_t width() const { return mWidth; }
    size_t height() const { return mHeight; }
    size_t scrollBackLength() const { return mScrollback.size(); }
    size_t widestLine() const { return mWidestLine; }

    void scroll(size_t left, size_t top);
    void resize(size_t width, size_t height);
    void render();

    enum Event {
        Update,
        ScrollbackChanged,
        LineChanged
    };

    virtual void event(Event, void *data = nullptr) = 0;
    enum RenderFlag {
        Render_None = 0,
        Render_Selected = (1ull << 1)
    };
    virtual void render(size_t y, size_t x, const char *ch, size_t len, unsigned int flags) = 0;

    void addText(const std::string &string);

    void keyPressEvent(const KeyEvent &event);
    void keyReleaseEvent(const KeyEvent &event);
    void mousePressEvent(const MouseEvent &event);
    void mouseReleaseEvent(const MouseEvent &event);
    void mouseMoveEvent(const MouseEvent &event);
private:
    bool isSelected(size_t y, size_t *start, size_t *length) const;
private:
    size_t mX { 0 }, mY { 0 }, mWidth { 0 }, mHeight { 0 };
    unsigned int mFlags { 0 };
    std::vector<std::string> mScrollback;
    bool mHasSelection { false };
    size_t mSelectionStartX { 0 }, mSelectionStartY { 0 }, mSelectionEndX { 0 }, mSelectionEndY { 0 };
    size_t mWidestLine { 0 };
};

#endif /* TERMINAL_H */
