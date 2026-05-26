#include "Window_wayland.h"

#include <spdlog/spdlog.h>

WaylandWindow::WaylandWindow(EventLoop &loop)
    : loop_(loop)
{
}

WaylandWindow::~WaylandWindow() = default;

bool WaylandWindow::create(int /*width*/, int /*height*/, const std::string & /*title*/)
{
    // Stage 0: placeholder. Real wl_display_connect / xdg_toplevel
    // bring-up lands in Stage 1 (see WAYLAND.md). Log at error level so
    // the workaround is visible at default verbosity — the caller
    // (PlatformDawn) doesn't know what backend it constructed.
    spdlog::error("WaylandWindow is a Stage 1 placeholder and cannot create a window yet.");
    spdlog::error("Relaunch with `mb --x11` to use the X11/XWayland backend in the meantime.");
    return false;
}

void WaylandWindow::destroy()
{
}
