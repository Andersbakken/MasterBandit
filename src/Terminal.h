#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>

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

    void scroll(size_t left, size_t top);
    void resize(size_t width, size_t height);
    void render();

    enum Event {
        Update,
        ScrollbackChanged,
        LineChanged
    };

    virtual void event(Event, void *data = nullptr) = 0;
    virtual void render(size_t y, const std::string &line) = 0;

    void addText(const std::string &string);
private:
    size_t mX { 0 }, mY { 0 }, mWidth { 0 }, mHeight { 0 };
    unsigned int mFlags { 0 };
    std::vector<std::string> mScrollback;
};

#endif /* TERMINAL_H */
