#pragma once

#include <EventLoop.h>
#include <Window.h>

#include <string>

// Placeholder Wayland Window backend.
//
// Compiles unconditionally on Linux so the Window factory in PlatformDawn
// has a candidate to construct on Wayland sessions. create() currently
// returns false, which causes PlatformDawn to log + bail (the existing
// soft-fail path at PlatformDawn.cpp:1188), and the runtime selection
// logic will fall back to the XCB backend.
//
// Real implementation lands in Stage 1 of the Wayland port — see
// WAYLAND.md for the staged plan.
class WaylandWindow : public Window
{
public:
    explicit WaylandWindow(EventLoop &loop);
    ~WaylandWindow() override;

    bool create(int width, int height, const std::string &title) override;
    void destroy() override;

    bool shouldClose() const override { return true; }

    void setTitle(const std::string &title) override { (void)title; }

    void getFramebufferSize(int &w, int &h) const override { w = h = 0; }

    void getContentScale(float &x, float &y) const override { x = y = 1.0f; }

    void setClipboard(const std::string &text) override { (void)text; }

    void requestSelection(SelectionSource src, SelectionCallback cb) override
    {
        (void)src;
        if (cb) {
            cb(std::nullopt);
        }
    }

    wgpu::Surface createWgpuSurface(wgpu::Instance instance) override
    {
        (void)instance;
        return nullptr;
    }

private:
    EventLoop &loop_;
};
