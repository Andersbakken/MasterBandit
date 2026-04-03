#include "Tab.h"

Tab::Tab(std::unique_ptr<Layout> layout)
    : layout_(std::move(layout))
{}

void Tab::pushOverlay(std::unique_ptr<Terminal> t)
{
    overlays_.push_back(std::move(t));
}

void Tab::popOverlay()
{
    if (overlays_.empty()) return;
    int fd = overlays_.back()->masterFD();
    overlays_.pop_back();
    if (onOverlayPopped) onOverlayPopped(fd);
}

Terminal* Tab::topOverlay()
{
    return overlays_.empty() ? nullptr : overlays_.back().get();
}

Terminal* Tab::activeOverlay()
{
    return overlays_.empty() ? nullptr : overlays_.back().get();
}
