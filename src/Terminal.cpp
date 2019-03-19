#include "Terminal.h"
#include <assert.h>

Terminal::Terminal()
{
}

Terminal::~Terminal()
{
}

void Terminal::scroll(size_t x, size_t y)
{
    mX = x;
    mY = y;
    event(Update);
}

void Terminal::resize(size_t width, size_t height)
{
    mWidth = width;
    mHeight = height;
    event(Update);
}

void Terminal::render()
{
    const size_t max = std::min(mY + mHeight, mScrollback.size());
    for (size_t y = mY; y<max; ++y) {
        // printf("RENDERING %s\n", mScrollback.at(y).c_str());
        render(y - mY, mScrollback.at(y));
    }
}

void Terminal::addText(const std::string &string)
{
    size_t last = 0;
    const char *str = string.c_str();
    for (size_t idx = 0; idx<string.size(); ++idx) {
        if (*str++ == '\n') {
            mScrollback.push_back(string.substr(last, idx - last));
            last = idx + 1;
        }
    }

    if (last < string.size()) {
        mScrollback.push_back(string.substr(last));
    }
    event(Update);
    event(ScrollbackChanged);
}
