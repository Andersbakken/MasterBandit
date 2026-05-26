#pragma once

#include <EventLoop.h>
#include <Window.h>

#include <cstdint>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

// Stage 1 Wayland backend. Opens a wl_surface + xdg_toplevel, waits for the
// first configure, dispatches Wayland events from the EpollEventLoop fd, and
// hands the surface to Dawn via SurfaceSourceWaylandSurface.
//
// Stage 2+ (keyboard, pointer, clipboard, scale) lands in later patches; the
// stubs in this header return empty / no-op until then.
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

    // Registry listener trampolines
    static void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version);
    static void onRegistryGlobalRemove(void *data, wl_registry *registry, uint32_t name);

    // xdg_wm_base ping → pong
    static void onXdgWmBasePing(void *data, xdg_wm_base *base, uint32_t serial);

    // xdg_surface configure → ack_configure (+ fire onFramebufferResize on first
    // configure or when pending size differs from committed size).
    static void onXdgSurfaceConfigure(void *data, xdg_surface *surface, uint32_t serial);

    // xdg_toplevel listener: configure (latch pending size), close (shouldClose),
    // configure_bounds / wm_capabilities (no-op for Stage 1).
    static void onXdgToplevelConfigure(void *data, xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states);
    static void onXdgToplevelClose(void *data, xdg_toplevel *toplevel);
    static void onXdgToplevelConfigureBounds(void *data, xdg_toplevel *toplevel, int32_t width, int32_t height);
    static void onXdgToplevelWmCapabilities(void *data, xdg_toplevel *toplevel, struct wl_array *capabilities);

    // Drive the libwayland dispatch state machine from the epoll callback.
    // Pattern is the canonical prepare_read / read_events / dispatch_pending
    // sequence from the wayland-book; see processEvents() in the .cpp for the
    // rationale on each step.
    void dispatchWayland();

    EventLoop &loop_;

    wl_display *display_       = nullptr;
    wl_registry *registry_     = nullptr;
    wl_compositor *compositor_ = nullptr;
    xdg_wm_base *wmBase_       = nullptr;

    wl_surface *surface_     = nullptr;
    xdg_surface *xdgSurface_ = nullptr;
    xdg_toplevel *toplevel_  = nullptr;

    int wlFd_ = -1;

    // Pending size from the latest xdg_toplevel.configure. 0 means "client
    // picks"; we use defaultWidth_ / defaultHeight_ in that case. Committed
    // size is reported by getFramebufferSize() and tracks the last value
    // delivered to onFramebufferResize.
    int pendingWidth_  = 0;
    int pendingHeight_ = 0;
    int width_         = 0;
    int height_        = 0;
    int defaultWidth_  = 800;
    int defaultHeight_ = 600;

    // Set true after the first xdg_surface.configure has been ack'd.
    // create() roundtrips to drive the first configure synchronously and
    // returns with width_/height_ populated; subsequent configures (user
    // resize) fire onFramebufferResize from the dispatch path.
    bool firstConfigure_ = false;
    bool shouldClose_    = false;
};
