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
        size_t start, length;
        const std::string &line = mScrollback[y];
        if (isSelected(y, &start, &length)) {
            assert(start + length <= line.size());
            if (start > 0)
                render(0, y - mY, line.c_str(), start, Render_None);
            render(start, y - mY, line.c_str() + start, length, Render_Selected);
            if (start + length < line.size())
                render(start + length, y - mY, line.c_str() + start + length, line.size() - start - length, Render_None);
        } else {
            render(0, y - mY, line.c_str(), line.size(), Render_None);
        }
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

bool Terminal::isSelected(size_t y, size_t *start, size_t *length) const
{
    if (!mHasSelection && (mSelectionStartY != mSelectionEndY || mSelectionStartX != mSelectionEndX)) {
        *start = *length = 0;
        return false;
    }
    const size_t minY = std::min(mSelectionStartY, mSelectionEndY);
    const size_t maxY = std::max(mSelectionStartY, mSelectionEndY);
    const size_t minX = std::min(mSelectionStartX, mSelectionEndX);
    const size_t maxX = std::max(mSelectionStartX, mSelectionEndX);

    if (y > minY && y < maxY) {
        *start = 0;
        *length = mScrollback[y].length();
    } else if (y == minY && y == maxY) {
        *start = minX;
        *length = mScrollback[y].length() - minX - maxX;
    } else if (y == minY) {
        *start = minX;
        *length = mScrollback[y].length() - minX;
    } else if (y == maxY) {
        *start = 0;
        *length = mScrollback[y].length() - maxX;
    } else {
        *start = *length = 0;
        return false;
    }
    return true;
}

void Terminal::keyPressEvent(const KeyEvent &event)
{
}

void Terminal::keyReleaseEvent(const KeyEvent &event)
{
}

void Terminal::mousePressEvent(const MouseEvent &event)
{
}

void Terminal::mouseReleaseEvent(const MouseEvent &event)
{
}

void Terminal::mouseMoveEvent(const MouseEvent &event)
{
}
