#include "Tab.h"

Tab::Tab(std::unique_ptr<Layout> layout)
    : layout_(std::move(layout))
{}

void Tab::pushOverlay(std::unique_ptr<Terminal> t)
{
    overlays_.push_back(std::move(t));
}

std::unique_ptr<Terminal> Tab::popOverlay()
{
    if (overlays_.empty()) return nullptr;
    std::unique_ptr<Terminal> t = std::move(overlays_.back());
    overlays_.pop_back();
    if (t && onOverlayPopped) onOverlayPopped(t->masterFD());
    return t;
}

Terminal* Tab::topOverlay()
{
    return overlays_.empty() ? nullptr : overlays_.back().get();
}

Terminal* Tab::activeOverlay()
{
    return overlays_.empty() ? nullptr : overlays_.back().get();
}
