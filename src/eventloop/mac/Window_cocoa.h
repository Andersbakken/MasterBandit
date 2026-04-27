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
    void raise() override;

    void setTitle(const std::string& title) override;
    void getFramebufferSize(int& w, int& h) const override;
    void getContentScale(float& x, float& y) const override;
    void getScreenSize(int& w, int& h) const override;

    void        setClipboard(const std::string& text) override;
    std::string getClipboard() const override;

    std::string keyName(int keycode) const override;
    uint32_t shiftedKeyCodepoint(int keycode) const override;

    void setAltSendsEsc(bool v) override { altSendsEsc_ = v; }
    bool altSendsEsc() const { return altSendsEsc_; }

    wgpu::Surface createWgpuSurface(wgpu::Instance instance) override;
    void setCursorStyle(CursorStyle shape) override;

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
    void dispatchExpose();

    // Called from MBView's setFrameSize: each time AppKit reports a new
    // size during a drag. Marks live-resize as in-progress and (re)starts
    // the gap-based debounce timer; SIGWINCH stays suppressed until the
    // timer fires (~100ms with no further size changes) or the drag ends.
    void noteResizeDuringDrag();
    // Called from MBView's viewDidEndLiveResize. Cancels the debounce and
    // fires onLiveResizeEnd immediately so the SIGWINCH lands on mouse-up
    // rather than after the 100ms grace period.
    void endLiveResize();

    // Internal — used by the CFRunLoopTimer callback to release its own ref.
    void cancelResizeDebounce();

    // Exposes the underlying NSWindow* for the macOS platform layer
    // (notification gating needs it for focus/occlusion queries). Returns
    // nullptr before create() / after destroy().
    NSWindow* nsWindowRaw() const;

private:
    NSWindow*   nsWindow_   = nullptr;
    MBView*     mbView_     = nullptr;
    bool        shouldClose_ = false;
    bool        altSendsEsc_ = true;
    // CFRunLoopTimer (not the EventLoop's kqueue-based timer) so the
    // debounce fires during AppKit's tracking-mode loop while the user
    // is dragging the window. The kqueue path can be starved by AppKit
    // event processing and never fire until mouse-up; CFRunLoopTimer is
    // serviced directly by the run loop in commonModes (which includes
    // NSEventTrackingRunLoopMode), so the 100 ms idle gap actually wakes
    // us mid-drag the way XCB's epoll-driven timer does on Linux.
    void*       resizeDebounceTimer_ = nullptr;  // CFRunLoopTimerRef
};
