#pragma once

#include <EventLoop.h>
#include <Window.h>

#include <cstdint>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_keyboard;
struct wl_surface;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// Stage 1-2 Wayland backend. Opens a wl_surface + xdg_toplevel, dispatches
// Wayland events from the EpollEventLoop fd, hands the surface to Dawn via
// SurfaceSourceWaylandSurface (Stage 1), and processes keyboard input
// (xkbcommon-backed keymap, modifiers, client-synthesized key repeat) via
// wl_seat + wl_keyboard (Stage 2).
//
// Stage 3+ (pointer, clipboard, scale) lands in later patches.
class WaylandWindow : public Window
{
public:
    explicit WaylandWindow(EventLoop &loop);
    ~WaylandWindow() override;

    bool create(int width, int height, const std::string &title) override;
    void destroy() override;

    bool shouldClose() const override { return shouldClose_; }

    void setTitle(const std::string &title) override;
    void getFramebufferSize(int &w, int &h) const override;
    void getContentScale(float &x, float &y) const override;
    void getScreenSize(int &w, int &h) const override;

    void setClipboard(const std::string & /*text*/) override { }

    void requestSelection(SelectionSource /*src*/, SelectionCallback cb) override
    {
        if (cb) {
            cb(std::nullopt);
        }
    }

    std::string keyName(int keycode) const override;
    uint32_t shiftedKeyCodepoint(int keycode) const override;
    uint32_t baseLayoutKeyCodepoint(int keycode) const override;

    wgpu::Surface createWgpuSurface(wgpu::Instance instance) override;

    // Called by EpollEventLoop when wl_display fd is readable.
    void processEvents();

private:
    // Listener vtables — defined in the .cpp. Declared as static members so
    // the static trampolines below can stay private.
    static const struct wl_registry_listener kRegistryListener;
    static const struct xdg_wm_base_listener kWmBaseListener;
    static const struct xdg_surface_listener kXdgSurfaceListener;
    static const struct xdg_toplevel_listener kToplevelListener;
    static const struct wl_seat_listener kSeatListener;
    static const struct wl_keyboard_listener kKeyboardListener;

    static void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version);
    static void onRegistryGlobalRemove(void *data, wl_registry *registry, uint32_t name);
    static void onXdgWmBasePing(void *data, xdg_wm_base *base, uint32_t serial);
    static void onXdgSurfaceConfigure(void *data, xdg_surface *surface, uint32_t serial);
    static void onXdgToplevelConfigure(void *data, xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
    static void onXdgToplevelClose(void *data, xdg_toplevel *toplevel);
    static void onXdgToplevelConfigureBounds(void *data, xdg_toplevel *toplevel, int32_t width, int32_t height);
    static void onXdgToplevelWmCapabilities(void *data, xdg_toplevel *toplevel, struct wl_array *capabilities);

    static void onSeatCapabilities(void *data, wl_seat *seat, uint32_t capabilities);
    static void onSeatName(void *data, wl_seat *seat, const char *name);

    static void onKeyboardKeymap(void *data, wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
    static void onKeyboardEnter(void *data, wl_keyboard *keyboard, uint32_t serial, wl_surface *surface, struct wl_array *keys);
    static void onKeyboardLeave(void *data, wl_keyboard *keyboard, uint32_t serial, wl_surface *surface);
    static void onKeyboardKey(void *data, wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    static void onKeyboardModifiers(void *data, wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
    static void onKeyboardRepeatInfo(void *data, wl_keyboard *keyboard, int32_t rate, int32_t delay);

    // Drive the libwayland dispatch state machine from the epoll callback.
    // Pattern is the canonical prepare_read / read_events / dispatch_pending
    // sequence from the wayland-book; see processEvents() in the .cpp for the
    // rationale on each step.
    void dispatchWayland();

    // Common keyboard dispatch helpers.
    void dispatchKey(uint32_t xkbKeycode, bool isRepeat);
    void dispatchKeyRelease(uint32_t xkbKeycode);
    void startKeyRepeat(uint32_t xkbKeycode);
    void cancelKeyRepeat();
    void ensureDefaultKeymap();

    EventLoop &loop_;

    wl_display *display_       = nullptr;
    wl_registry *registry_     = nullptr;
    wl_compositor *compositor_ = nullptr;
    xdg_wm_base *wmBase_       = nullptr;
    wl_seat *seat_             = nullptr;
    wl_keyboard *keyboard_     = nullptr;

    wl_surface *surface_     = nullptr;
    xdg_surface *xdgSurface_ = nullptr;
    xdg_toplevel *toplevel_  = nullptr;

    int wlFd_ = -1;

    int pendingWidth_  = 0;
    int pendingHeight_ = 0;
    int width_         = 0;
    int height_        = 0;
    int defaultWidth_  = 800;
    int defaultHeight_ = 600;

    bool firstConfigure_ = false;
    bool shouldClose_    = false;

    // xkbcommon state. xkbCtx_ owns the keymap+state lifetimes; the keymap
    // and primary state are replaced whenever wl_keyboard.keymap fires.
    // xkbCleanState_ tracks the layout group only (no modifiers) for the
    // kitty CSI-u unshifted-codepoint field. xkbDefaultKeymap_ is built
    // lazily from empty rule_names so baseLayoutKeyCodepoint reports the
    // system-default ("us") layout regardless of the user's current layout.
    xkb_context *xkbCtx_          = nullptr;
    xkb_keymap *xkbKeymap_        = nullptr;
    xkb_state *xkbState_          = nullptr;
    xkb_state *xkbCleanState_     = nullptr;
    xkb_keymap *xkbDefaultKeymap_ = nullptr;
    xkb_state *xkbDefaultState_   = nullptr;

    // Client-synthesized key repeat. Wayland delivers `repeat_info` (rate +
    // initial delay) but the client is responsible for re-emitting the key
    // events. Active-repeat key is tracked so the next press / release /
    // focus-out cancels cleanly. rate_ in keys/s; delay_ in ms; both 0
    // means "no repeat" (per wl_keyboard.repeat_info spec).
    int32_t repeatRate_             = 0;
    int32_t repeatDelay_            = 0;
    uint32_t repeatKeycode_         = 0;
    EventLoop::TimerId repeatTimer_ = 0;
};
