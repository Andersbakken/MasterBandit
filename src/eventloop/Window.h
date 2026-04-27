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
    virtual uint32_t shiftedKeyCodepoint(int keycode) const { (void)keycode; return 0; }

    // When true, the macOS backend skips NSTextInputClient processing for
    // Option+<letter> so the OS doesn't compose the "option character" (∫,
    // ƒ, etc.) — the terminal will send ESC+<letter> via the legacy key
    // path instead. No-op on platforms where Alt+letter already reaches
    // onChar as a plain character (Linux/XCB).
    virtual void setAltSendsEsc(bool v) { (void)v; }

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
    // Visibility tracks the kitty `os_window_is_invisible` shape (glfw.c:2563-2571):
    // visible iff mapped && !iconified && !fully-obscured. Backends fire this
    // when any of those inputs change. Used by OSC 99 `o=invisible` gating in
    // PlatformUtils_Linux.cpp; macOS reads NSWindow.occlusionState directly
    // and ignores this hook.
    std::function<void(bool visible)> onVisibility;
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

    // Live resize state — true while user is actively dragging a window edge.
    // macOS: set by window delegate callbacks; Linux: debounced via timestamp.
    virtual bool inLiveResize() const { return inLiveResize_; }

    // Best-effort window activation. Driven by OSC 99 a=focus when the
    // user clicks a notification. Each backend implements as much as the
    // window system allows:
    //   xcb : _NET_ACTIVE_WINDOW source=2 (pager) + xcb_set_input_focus
    //   wayland (future): xdg_activation_v1.activate(token, surface)
    //   macOS (future): [NSApp activateIgnoringOtherApps:YES] + makeKeyAndOrderFront
    // Compositors may demote to an urgency hint (taskbar bounce / tab
    // highlight) instead of granting focus — that's the user-side policy
    // and not something we can override from the requesting side.
    virtual void raise() {}

protected:
    bool inLiveResize_ = false;
};
