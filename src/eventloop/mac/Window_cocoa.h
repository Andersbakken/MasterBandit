#pragma once

#include <Window.h>

#include <dawn/webgpu_cpp.h>

#include <string>

#ifdef __OBJC__
@class NSWindow;
@class MBView;
#else
struct objc_object;
using NSWindow = objc_object;
using MBView   = objc_object;
#endif

class CocoaWindow : public Window {
public:
    CocoaWindow();
    ~CocoaWindow() override;

    bool create(int width, int height, const std::string& title) override;
    void destroy() override;
    bool shouldClose() const override { return shouldClose_; }

    void setTitle(const std::string& title) override;
    void getFramebufferSize(int& w, int& h) const override;
    void getContentScale(float& x, float& y) const override;

    void        setClipboard(const std::string& text) override;
    std::string getClipboard() const override;

    std::string keyName(int keycode) const override;

    wgpu::Surface createWgpuSurface(wgpu::Instance instance) override;

    // Called by MBView / MBWindowDelegate for input events
    void dispatchKey(int key, int scancode, int action, int mods);
    void dispatchChar(uint32_t codepoint);
    void dispatchMouseButton(int button, int action, int mods);
    void dispatchCursorPos(double x, double y);
    void dispatchScroll(double dx, double dy);
    void dispatchFocus(bool focused);
    void dispatchResize(int w, int h);
    void dispatchContentScale(float sx, float sy);
    void dispatchClose();

private:
    NSWindow* nsWindow_  = nullptr;
    MBView*   mbView_    = nullptr;
    bool      shouldClose_ = false;
};
