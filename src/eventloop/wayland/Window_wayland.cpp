#include "Window_wayland.h"

#include <dawn/webgpu_cpp.h>
#include <spdlog/spdlog.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

// Application identifier used for xdg_toplevel.set_app_id and WM_CLASS on
// X11. Compositors / notification daemons use this string to attribute
// activity to a desktop entry.
constexpr const char *kAppId = "it.masterband.mb";

// Bind to the highest registry version the client knows about, clamped by
// the version the compositor advertised. Stage 1 only uses v1 entry points
// of each interface but binding higher avoids a future re-bind when later
// stages need newer methods.
constexpr uint32_t kCompositorClientVersion = 4;
constexpr uint32_t kXdgWmBaseClientVersion  = 5;

} // namespace

WaylandWindow::WaylandWindow(EventLoop &loop)
    : loop_(loop)
{
}

WaylandWindow::~WaylandWindow()
{
    destroy();
}

// ---------- listeners (static trampolines) ----------

const wl_registry_listener WaylandWindow::kRegistryListener = {
    .global        = &WaylandWindow::onRegistryGlobal,
    .global_remove = &WaylandWindow::onRegistryGlobalRemove,
};

const xdg_wm_base_listener WaylandWindow::kWmBaseListener = {
    .ping = &WaylandWindow::onXdgWmBasePing,
};

const xdg_surface_listener WaylandWindow::kXdgSurfaceListener = {
    .configure = &WaylandWindow::onXdgSurfaceConfigure,
};

const xdg_toplevel_listener WaylandWindow::kToplevelListener = {
    .configure        = &WaylandWindow::onXdgToplevelConfigure,
    .close            = &WaylandWindow::onXdgToplevelClose,
    .configure_bounds = &WaylandWindow::onXdgToplevelConfigureBounds,
    .wm_capabilities  = &WaylandWindow::onXdgToplevelWmCapabilities,
};

void WaylandWindow::onRegistryGlobal(void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        uint32_t v        = std::min(version, kCompositorClientVersion);
        self->compositor_ = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, v));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        uint32_t v    = std::min(version, kXdgWmBaseClientVersion);
        self->wmBase_ = static_cast<xdg_wm_base *>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, v));
        xdg_wm_base_add_listener(self->wmBase_, &kWmBaseListener, self);
    }
}

void WaylandWindow::onRegistryGlobalRemove(void *, wl_registry *, uint32_t)
{
    // Stage 1 ignores removals; later stages that bind dynamic globals
    // (data devices, outputs) will need to handle them here.
}

void WaylandWindow::onXdgWmBasePing(void *, xdg_wm_base *base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}

void WaylandWindow::onXdgSurfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
{
    auto *self = static_cast<WaylandWindow *>(data);
    xdg_surface_ack_configure(surface, serial);

    // Resolve pending size: 0 from toplevel.configure means "client picks".
    // First configure with 0/0 falls back to the create()-requested size;
    // subsequent 0/0 configures keep the existing size (compositor isn't
    // forcing a new geometry, just re-confirming).
    int newW = self->pendingWidth_;
    int newH = self->pendingHeight_;
    if (newW <= 0) {
        newW = self->firstConfigure_ ? self->width_ : self->defaultWidth_;
    }
    if (newH <= 0) {
        newH = self->firstConfigure_ ? self->height_ : self->defaultHeight_;
    }

    const bool sizeChanged = (newW != self->width_) || (newH != self->height_);
    self->width_           = newW;
    self->height_          = newH;

    if (!self->firstConfigure_) {
        // First configure: create() is blocked in wl_display_roundtrip and
        // will read width_/height_ after the roundtrip returns. Don't fire
        // the resize callback yet — PlatformDawn reads via getFramebufferSize.
        self->firstConfigure_ = true;
    } else if (sizeChanged && self->onFramebufferResize) {
        self->onFramebufferResize(self->width_, self->height_);
    }
}

void WaylandWindow::onXdgToplevelConfigure(void *data, xdg_toplevel *, int32_t width, int32_t height, struct wl_array *)
{
    auto *self           = static_cast<WaylandWindow *>(data);
    self->pendingWidth_  = width;
    self->pendingHeight_ = height;
}

void WaylandWindow::onXdgToplevelClose(void *data, xdg_toplevel *)
{
    auto *self         = static_cast<WaylandWindow *>(data);
    self->shouldClose_ = true;
}

void WaylandWindow::onXdgToplevelConfigureBounds(void *, xdg_toplevel *, int32_t, int32_t)
{
    // Compositor-suggested upper bound on toplevel size. Ignored in Stage 1.
}

void WaylandWindow::onXdgToplevelWmCapabilities(void *, xdg_toplevel *, struct wl_array *)
{
    // Advertised compositor capabilities (minimize/maximize/...). Ignored
    // until we expose those operations.
}

// ---------- lifecycle ----------

bool WaylandWindow::create(int width, int height, const std::string &title)
{
    defaultWidth_  = width > 0 ? width : 800;
    defaultHeight_ = height > 0 ? height : 600;

    display_ = wl_display_connect(nullptr);
    if (!display_) {
        spdlog::error("WaylandWindow: wl_display_connect failed (WAYLAND_DISPLAY={})",
                      std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "<unset>");
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    if (!registry_) {
        spdlog::error("WaylandWindow: wl_display_get_registry failed");
        destroy();
        return false;
    }
    wl_registry_add_listener(registry_, &kRegistryListener, this);

    // Roundtrip 1: receive the global advertisements so compositor_ and
    // wmBase_ are bound before we try to use them.
    if (wl_display_roundtrip(display_) < 0) {
        spdlog::error("WaylandWindow: wl_display_roundtrip (globals) failed");
        destroy();
        return false;
    }
    if (!compositor_) {
        spdlog::error("WaylandWindow: compositor missing wl_compositor global");
        destroy();
        return false;
    }
    if (!wmBase_) {
        spdlog::error("WaylandWindow: compositor missing xdg_wm_base global (xdg-shell required)");
        destroy();
        return false;
    }

    surface_ = wl_compositor_create_surface(compositor_);
    if (!surface_) {
        spdlog::error("WaylandWindow: wl_compositor_create_surface failed");
        destroy();
        return false;
    }

    xdgSurface_ = xdg_wm_base_get_xdg_surface(wmBase_, surface_);
    if (!xdgSurface_) {
        spdlog::error("WaylandWindow: xdg_wm_base_get_xdg_surface failed");
        destroy();
        return false;
    }
    xdg_surface_add_listener(xdgSurface_, &kXdgSurfaceListener, this);

    toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    if (!toplevel_) {
        spdlog::error("WaylandWindow: xdg_surface_get_toplevel failed");
        destroy();
        return false;
    }
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);

    xdg_toplevel_set_title(toplevel_, title.c_str());
    xdg_toplevel_set_app_id(toplevel_, kAppId);

    // Initial commit (no buffer) — required by xdg-shell before the
    // compositor will send the first configure.
    wl_surface_commit(surface_);

    // Roundtrip 2: drive the first xdg_surface.configure synchronously so
    // create() returns with width_/height_ populated for getFramebufferSize.
    if (wl_display_roundtrip(display_) < 0) {
        spdlog::error("WaylandWindow: wl_display_roundtrip (first configure) failed");
        destroy();
        return false;
    }
    if (!firstConfigure_) {
        spdlog::error("WaylandWindow: compositor did not send xdg_surface.configure after initial commit");
        destroy();
        return false;
    }

    // Register the wl_display fd with the event loop. dispatchWayland()
    // drives the libwayland state machine; processEvents() is the FdCb
    // hook for symmetry with XCBWindow::processEvents.
    wlFd_ = wl_display_get_fd(display_);
    loop_.watchFd(wlFd_, EventLoop::FdEvents::Readable, [this](EventLoop::FdEvents)
                  {
                      processEvents();
                  });

    spdlog::info("WaylandWindow: created {}x{} ({})", width_, height_, title);
    return true;
}

void WaylandWindow::destroy()
{
    if (wlFd_ >= 0) {
        loop_.removeFd(wlFd_);
        wlFd_ = -1;
    }
    // Destroy in reverse construction order; xdg_wm_base requires all of
    // its xdg_surfaces to be gone first. Registry-bound globals
    // (compositor, wmBase) and the registry itself are released by
    // wl_display_disconnect.
    if (toplevel_) {
        xdg_toplevel_destroy(toplevel_);
        toplevel_ = nullptr;
    }
    if (xdgSurface_) {
        xdg_surface_destroy(xdgSurface_);
        xdgSurface_ = nullptr;
    }
    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    wmBase_     = nullptr;
    compositor_ = nullptr;
    registry_   = nullptr;
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

// ---------- properties ----------

void WaylandWindow::setTitle(const std::string &title)
{
    if (toplevel_) {
        xdg_toplevel_set_title(toplevel_, title.c_str());
        if (display_) {
            wl_display_flush(display_);
        }
    }
}

void WaylandWindow::getFramebufferSize(int &w, int &h) const
{
    w = width_;
    h = height_;
}

void WaylandWindow::getContentScale(float &x, float &y) const
{
    // Stage 5 wires wp_fractional_scale_v1 + wl_output.scale; until then we
    // claim 1.0 to match the XCB backend.
    x = y = 1.0f;
}

void WaylandWindow::getScreenSize(int &w, int &h) const
{
    // No wl_output binding yet (Stage 5). Return 0 so PlatformDawn's clamp
    // falls through to the minimum texture-pool limit.
    w = h = 0;
}

// ---------- WebGPU surface ----------

wgpu::Surface WaylandWindow::createWgpuSurface(wgpu::Instance instance)
{
    if (!display_ || !surface_) {
        return nullptr;
    }
    wgpu::SurfaceSourceWaylandSurface waylandSource;
    waylandSource.display = display_;
    waylandSource.surface = surface_;

    wgpu::SurfaceDescriptor desc;
    desc.nextInChain = &waylandSource;
    return instance.CreateSurface(&desc);
}

// ---------- event processing ----------

void WaylandWindow::processEvents()
{
    dispatchWayland();
}

void WaylandWindow::dispatchWayland()
{
    if (!display_) {
        return;
    }

    // Canonical libwayland dispatch sequence for a single-threaded reader
    // driven by an external poll loop. Each step has a specific purpose:
    //
    //   * dispatch_pending: drain any events already deserialized into the
    //     per-thread queue from a prior wl_display_dispatch call. Without
    //     this, prepare_read may not be reachable (it fails while there are
    //     queued events).
    //   * prepare_read: declare the intent to read from the fd. If another
    //     thread is mid-read, this returns -1 and we yield to dispatch.
    //   * flush: push any outgoing requests to the compositor before we
    //     block-read; otherwise we deadlock waiting for events while the
    //     compositor waits for our request.
    //   * read_events: actually pull bytes from the fd into the queue (we
    //     are here because epoll said the fd is readable).
    //   * dispatch_pending: deliver the newly-queued events to listeners.
    if (wl_display_dispatch_pending(display_) < 0) {
        spdlog::error("WaylandWindow: wl_display_dispatch_pending failed: {}", std::strerror(errno));
        shouldClose_ = true;
        return;
    }
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) {
            spdlog::error("WaylandWindow: wl_display_dispatch_pending (loop) failed: {}", std::strerror(errno));
            shouldClose_ = true;
            return;
        }
    }
    if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
        spdlog::error("WaylandWindow: wl_display_flush failed: {}", std::strerror(errno));
        wl_display_cancel_read(display_);
        shouldClose_ = true;
        return;
    }
    if (wl_display_read_events(display_) < 0) {
        // EAGAIN is normal when epoll spuriously wakes us; everything else
        // means the compositor connection is broken.
        if (errno != EAGAIN) {
            spdlog::error("WaylandWindow: wl_display_read_events failed: {}", std::strerror(errno));
            shouldClose_ = true;
        }
        return;
    }
    if (wl_display_dispatch_pending(display_) < 0) {
        spdlog::error("WaylandWindow: wl_display_dispatch_pending (post-read) failed: {}", std::strerror(errno));
        shouldClose_ = true;
    }
}
