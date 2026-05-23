#pragma once

#include <dawn/webgpu_cpp.h>

#include <functional>
#include <optional>
#include <string>

class Window
{
public:
    virtual ~Window() = default;

    // Lifecycle
    virtual bool create(int width, int height, const std::string &title) = 0;
    virtual void destroy()                                               = 0;
    virtual bool shouldClose() const                                     = 0;

    // Properties
    virtual void setTitle(const std::string &title)        = 0;
    virtual void getFramebufferSize(int &w, int &h) const  = 0;
    virtual void getContentScale(float &x, float &y) const = 0;

    virtual void getScreenSize(int &w, int &h) const { w = h = 0; }

    // Clipboard. Writes are sync `void` because they don't block the caller
    // (XCB sets a selection owner; Cocoa writes NSPasteboard immediately).
    // Reads are exclusively through requestSelection — see below.
    virtual void setClipboard(const std::string &text) = 0;

    // X11 primary selection (drag-select / middle-click). No-op on non-X11.
    virtual void setPrimarySelection(const std::string &text) { (void)text; }

    // Async selection read. The callback fires exactly once on the main thread
    // with the resolved text or std::nullopt (refused/timed-out/no-owner).
    // The XCB backend round-trips through SELECTION_NOTIFY without blocking
    // the main thread; the Cocoa backend reads NSPasteboard inline and
    // invokes the callback synchronously since pasteboard reads are fast
    // and don't need an event-loop hop.
    enum class SelectionSource
    {
        Clipboard,
        Primary
    };
    using SelectionCallback                                                  = std::function<void(std::optional<std::string>)>;
    virtual void requestSelection(SelectionSource src, SelectionCallback cb) = 0;

    // Key name for a given platform key code (for Kitty keyboard protocol)
    virtual std::string keyName(int keycode) const
    {
        (void)keycode;
        return {};
    }

    virtual uint32_t shiftedKeyCodepoint(int keycode) const
    {
        (void)keycode;
        return 0;
    }

    // Codepoint this physical key would produce under the system default
    // (typically US ANSI) layout, regardless of the user's current layout.
    // Used to fill the kitty keyboard protocol's `base_layout_key` /
    // alternate_key field so non-US-layout users can still match physical
    // key shortcuts. Returns 0 if unavailable or identical to the
    // current-layout codepoint (encoder elides redundant emission).
    virtual uint32_t baseLayoutKeyCodepoint(int keycode) const
    {
        (void)keycode;
        return 0;
    }

    // Mirrors kitty's `macos_option_as_alt`. Selects which side(s) of the
    // Option key bypass macOS's Unicode composition (NSTextInputClient) so
    // the keystroke reaches the terminal as a real Alt modifier. The
    // bypassed side(s) cause the platform layer to OR `OptionAsAltModifier`
    // into the dispatched mods, which InputController gates ESC-prefix
    // emission on. Sides not bypassed go through normal macOS composition
    // (Option+E → dead key for accent, Option+B → ∫, etc.). No-op on
    // non-macOS platforms (Alt is unambiguous on X11/Wayland).
    enum class MacosOptionAsAlt : uint8_t
    {
        None,
        Left,
        Right,
        Both
    };

    virtual void setMacosOptionAsAlt(MacosOptionAsAlt v) { (void)v; }

    // Parse the config string. Recognized: "none"/"no"/"false" → None,
    // "left" → Left, "right" → Right, "both"/"yes"/"true" → Both. Unknown
    // values fall back to Left (the default).
    static MacosOptionAsAlt parseMacosOptionAsAlt(const std::string &s)
    {
        if (s == "none" || s == "no" || s == "false") {
            return MacosOptionAsAlt::None;
        }
        if (s == "right") {
            return MacosOptionAsAlt::Right;
        }
        if (s == "both" || s == "yes" || s == "true") {
            return MacosOptionAsAlt::Both;
        }
        return MacosOptionAsAlt::Left;
    }

    // WebGPU surface
    virtual wgpu::Surface createWgpuSurface(wgpu::Instance instance) = 0;

    // Input callbacks — set by PlatformDawn before create()
    // key/scancode/action/mods use platform-independent Key/Mod constants (see InputTypes.h)
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    // codepoint: the text the user typed (shift / dead-key composition / etc. all applied).
    // unshiftedCodepoint: same key with all modifiers stripped — used as the kitty CSI-u keyCode.
    // For Shift+A on US: codepoint='A' (0x41), unshiftedCodepoint='a' (0x61). 0 = unavailable.
    std::function<void(uint32_t codepoint, uint32_t unshiftedCodepoint)> onChar;
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
    enum class CursorStyle
    {
        Arrow,      // CSS "default"
        IBeam,      // CSS "text"
        Pointer,    // CSS "pointer" (hand)
        Crosshair,  // CSS "crosshair"
        Wait,       // CSS "wait"
        Help,       // CSS "help"
        Move,       // CSS "move"
        NotAllowed, // CSS "not-allowed"
        ResizeH,    // CSS "ew-resize" / e-/w-resize
        ResizeV,    // CSS "ns-resize" / n-/s-resize
        ResizeNESW, // CSS "nesw-resize"
        ResizeNWSE, // CSS "nwse-resize"
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
