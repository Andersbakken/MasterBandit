#pragma once

#include <dawn/webgpu_cpp.h>

#include <functional>
#include <string>

class Window {
public:
    virtual ~Window() = default;

    // Lifecycle
    virtual bool create(int width, int height, const std::string& title) = 0;
    virtual void destroy() = 0;
    virtual bool shouldClose() const = 0;

    // Properties
    virtual void setTitle(const std::string& title) = 0;
    virtual void getFramebufferSize(int& w, int& h) const = 0;
    virtual void getContentScale(float& x, float& y) const = 0;
    virtual void getScreenSize(int& w, int& h) const { w = h = 0; }

    // Clipboard
    virtual void        setClipboard(const std::string& text) = 0;
    virtual std::string getClipboard() const = 0;

    // X11 primary selection (middle-click paste) — no-op on non-X11
    virtual void        setPrimarySelection(const std::string& text) { (void)text; }
    virtual std::string getPrimarySelection() const { return {}; }

    // Key name for a given platform key code (for Kitty keyboard protocol)
    virtual std::string keyName(int keycode) const { (void)keycode; return {}; }

    // WebGPU surface
    virtual wgpu::Surface createWgpuSurface(wgpu::Instance instance) = 0;

    // Input callbacks — set by PlatformDawn before create()
    // key/scancode/action/mods use platform-independent Key/Mod constants (see InputTypes.h)
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    std::function<void(uint32_t codepoint)> onChar;
    std::function<void(int w, int h)> onFramebufferResize;
    std::function<void(float scaleX, float scaleY)> onContentScale;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(double x, double y)> onCursorPos;
    std::function<void(double dx, double dy)> onScroll;
    std::function<void(bool focused)> onFocus;
    std::function<void()> onExpose;        // called when window content needs redraw
    std::function<void()> onLiveResizeEnd; // called when live resize settles
    // Mouse cursor style
    enum class CursorStyle {
        Arrow,        // CSS "default"
        IBeam,        // CSS "text"
        Pointer,      // CSS "pointer" (hand)
        Crosshair,    // CSS "crosshair"
        Wait,         // CSS "wait"
        Help,         // CSS "help"
        Move,         // CSS "move"
        NotAllowed,   // CSS "not-allowed"
        ResizeH,      // CSS "ew-resize" / e-/w-resize
        ResizeV,      // CSS "ns-resize" / n-/s-resize
        ResizeNESW,   // CSS "nesw-resize"
        ResizeNWSE,   // CSS "nwse-resize"
    };
    virtual void setCursorStyle(CursorStyle) {}

    virtual void frameRendered() {}       // called after each frame; used for _NET_WM_SYNC_REQUEST

    // Live resize state — true while user is actively dragging a window edge.
    // macOS: set by window delegate callbacks; Linux: debounced via timestamp.
    virtual bool inLiveResize() const { return inLiveResize_; }

protected:
    bool inLiveResize_ = false;
};
