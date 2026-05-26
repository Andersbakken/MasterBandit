#include "Window_wayland.h"

#include <InputTypes.h>
#include <xkb/Xkb.h>

#include <dawn/webgpu_cpp.h>
#include <spdlog/spdlog.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

#include <sys/mman.h>
#include <unistd.h>

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
// wl_seat v5 added `name`; v4 added `wl_keyboard.repeat_info`. v7 is the
// current upstream version and adds `wl_pointer.frame` etc. Bind v7 if the
// compositor advertises it so Stage 3 (pointer) doesn't need a re-bind.
constexpr uint32_t kSeatClientVersion       = 7;

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

const wl_seat_listener WaylandWindow::kSeatListener = {
    .capabilities = &WaylandWindow::onSeatCapabilities,
    .name         = &WaylandWindow::onSeatName,
};

const wl_keyboard_listener WaylandWindow::kKeyboardListener = {
    .keymap      = &WaylandWindow::onKeyboardKeymap,
    .enter       = &WaylandWindow::onKeyboardEnter,
    .leave       = &WaylandWindow::onKeyboardLeave,
    .key         = &WaylandWindow::onKeyboardKey,
    .modifiers   = &WaylandWindow::onKeyboardModifiers,
    .repeat_info = &WaylandWindow::onKeyboardRepeatInfo,
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
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        uint32_t v  = std::min(version, kSeatClientVersion);
        self->seat_ = static_cast<wl_seat *>(
            wl_registry_bind(registry, name, &wl_seat_interface, v));
        wl_seat_add_listener(self->seat_, &kSeatListener, self);
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

// ---------- seat / keyboard ----------

void WaylandWindow::onSeatCapabilities(void *data, wl_seat *seat, uint32_t capabilities)
{
    auto *self        = static_cast<WaylandWindow *>(data);
    const bool hasKbd = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (hasKbd && !self->keyboard_) {
        self->keyboard_ = wl_seat_get_keyboard(seat);
        if (self->keyboard_) {
            wl_keyboard_add_listener(self->keyboard_, &kKeyboardListener, self);
        }
    } else if (!hasKbd && self->keyboard_) {
        self->cancelKeyRepeat();
        // wl_keyboard.release was added in wl_seat v3; we always bind ≥v5.
        wl_keyboard_release(self->keyboard_);
        self->keyboard_ = nullptr;
    }
}

void WaylandWindow::onSeatName(void *, wl_seat *, const char *)
{
    // Human-readable seat name (e.g. "seat0"). Unused.
}

void WaylandWindow::onKeyboardKeymap(void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        spdlog::warn("WaylandWindow: unsupported keymap format {}", format);
        close(fd);
        return;
    }

    // Compositor passes ownership of the fd; we must close it after mmap.
    // MAP_PRIVATE with PROT_READ is the documented invocation. Some
    // compositors (mutter) only mmap-as-private will work; MAP_SHARED can
    // fail with EACCES on the sealed memfd.
    void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        spdlog::error("WaylandWindow: mmap of keymap fd failed: {}", std::strerror(errno));
        close(fd);
        return;
    }

    // Release any prior keymap/state — keymap can be re-delivered when the
    // user changes layout or plugs in a new keyboard.
    if (self->xkbState_) {
        xkb_state_unref(self->xkbState_);
        self->xkbState_ = nullptr;
    }
    if (self->xkbCleanState_) {
        xkb_state_unref(self->xkbCleanState_);
        self->xkbCleanState_ = nullptr;
    }
    if (self->xkbKeymap_) {
        xkb_keymap_unref(self->xkbKeymap_);
        self->xkbKeymap_ = nullptr;
    }

    self->xkbKeymap_ = xkb_keymap_new_from_string(self->xkbCtx_,
                                                  static_cast<const char *>(map),
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);

    if (!self->xkbKeymap_) {
        spdlog::error("WaylandWindow: xkb_keymap_new_from_string failed");
        return;
    }
    self->xkbState_ = xkb_state_new(self->xkbKeymap_);
    if (!self->xkbState_) {
        spdlog::error("WaylandWindow: xkb_state_new failed");
        return;
    }
    // Clean state: modifiers always zero, layout group tracked by the
    // modifiers handler. Used for the kitty CSI-u unshifted codepoint.
    self->xkbCleanState_ = xkb_state_new(self->xkbKeymap_);
    if (!self->xkbCleanState_) {
        spdlog::warn("WaylandWindow: xkb_state_new(clean) failed; unshifted keyCode disabled");
    }
}

void WaylandWindow::onKeyboardEnter(void *data, wl_keyboard *, uint32_t, wl_surface *, struct wl_array *)
{
    auto *self = static_cast<WaylandWindow *>(data);
    // The `keys` array carries the keycodes currently held when focus
    // arrives. We rely on the modifiers event (sent immediately after enter)
    // to resync the modifier mask; per-key state isn't needed yet because
    // we don't track held keys for binding chord detection in this layer.
    if (self->onFocus) {
        self->onFocus(true);
    }
}

void WaylandWindow::onKeyboardLeave(void *data, wl_keyboard *, uint32_t, wl_surface *)
{
    auto *self = static_cast<WaylandWindow *>(data);
    self->cancelKeyRepeat();
    if (self->onFocus) {
        self->onFocus(false);
    }
}

void WaylandWindow::onKeyboardKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (!self->xkbState_ || !self->xkbKeymap_) {
        return;
    }
    // Wayland delivers raw evdev keycodes; xkbcommon uses the X11 convention
    // of +8 (the legacy keyboard-extension offset). This is the documented
    // mapping and matches every other Wayland client.
    const uint32_t kc = key + 8;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        xkb_state_update_key(self->xkbState_, kc, XKB_KEY_DOWN);
        self->dispatchKey(kc, /*isRepeat=*/false);
        self->startKeyRepeat(kc);
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        xkb_state_update_key(self->xkbState_, kc, XKB_KEY_UP);
        self->dispatchKeyRelease(kc);
        if (kc == self->repeatKeycode_) {
            self->cancelKeyRepeat();
        }
    }
}

void WaylandWindow::onKeyboardModifiers(void *data, wl_keyboard *, uint32_t, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (self->xkbState_) {
        xkb_state_update_mask(self->xkbState_, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
    if (self->xkbCleanState_) {
        // Clean state stays modifier-free; only the layout group is synced
        // so xkb_state_key_get_utf32(clean, kc) yields the unshifted
        // codepoint in the user's current layout.
        xkb_state_update_mask(self->xkbCleanState_, 0, 0, 0, 0, 0, group);
    }
}

void WaylandWindow::onKeyboardRepeatInfo(void *data, wl_keyboard *, int32_t rate, int32_t delay)
{
    auto *self         = static_cast<WaylandWindow *>(data);
    self->repeatRate_  = rate;
    self->repeatDelay_ = delay;
    // rate == 0 per spec means "no repeat"; cancel anything in flight so we
    // don't keep firing after the compositor disables it.
    if (rate == 0) {
        self->cancelKeyRepeat();
    }
}

void WaylandWindow::dispatchKey(uint32_t xkbKeycode, bool isRepeat)
{
    Key k         = mb::xkb::keysymToKey(mb::xkb::baseKeysymForKeycode(xkbState_, xkbKeymap_, xkbKeycode));
    uint32_t mods = mb::xkb::stateToModifiers(xkbState_);
    KeyAction act = isRepeat ? KeyAction_Repeat : KeyAction_Press;
    if (onKey) {
        onKey(static_cast<int>(k), static_cast<int>(xkbKeycode), static_cast<int>(act), static_cast<int>(mods));
    }
    if (onChar && !(mods & CtrlModifier) && !(mods & MetaModifier)) {
        uint32_t cp = xkb_state_key_get_utf32(xkbState_, xkbKeycode);
        if (cp >= 0x20 && cp != 0x7f) {
            uint32_t unshifted = xkbCleanState_ ? xkb_state_key_get_utf32(xkbCleanState_, xkbKeycode) : 0;
            if (unshifted < 0x20 || unshifted == 0x7f) {
                unshifted = 0;
            }
            onChar(cp, unshifted);
        }
    }
}

void WaylandWindow::dispatchKeyRelease(uint32_t xkbKeycode)
{
    Key k         = mb::xkb::keysymToKey(mb::xkb::baseKeysymForKeycode(xkbState_, xkbKeymap_, xkbKeycode));
    uint32_t mods = mb::xkb::stateToModifiers(xkbState_);
    if (onKey) {
        onKey(static_cast<int>(k), static_cast<int>(xkbKeycode), static_cast<int>(KeyAction_Release), static_cast<int>(mods));
    }
}

void WaylandWindow::startKeyRepeat(uint32_t xkbKeycode)
{
    cancelKeyRepeat();
    if (repeatRate_ <= 0 || repeatDelay_ <= 0 || !xkbKeymap_) {
        return;
    }
    if (!xkb_keymap_key_repeats(xkbKeymap_, xkbKeycode)) {
        return;
    }
    repeatKeycode_          = xkbKeycode;
    // Initial delay (one-shot). When that fires, emit one repeat and
    // re-arm as a periodic timer at 1000/rate ms.
    const uint64_t periodMs = 1000ull / static_cast<uint64_t>(repeatRate_);
    repeatTimer_            = loop_.addTimer(static_cast<uint64_t>(repeatDelay_), false, [this, periodMs]()
                                             {
                                      // Drop the one-shot ID; it has already fired.
                                      repeatTimer_ = 0;
                                      dispatchKey(repeatKeycode_, /*isRepeat=*/true);
                                      // Re-arm as periodic for subsequent fires. Use the
                                      // captured period so a mid-repeat repeat_info change
                                      // doesn't surprise this active session.
                                      repeatTimer_ = loop_.addTimer(periodMs, true, [this]()
                                                                    {
                                                                        dispatchKey(repeatKeycode_, /*isRepeat=*/true);
                                                                    });
                                             });
}

void WaylandWindow::cancelKeyRepeat()
{
    if (repeatTimer_) {
        loop_.removeTimer(repeatTimer_);
        repeatTimer_ = 0;
    }
    repeatKeycode_ = 0;
}

void WaylandWindow::ensureDefaultKeymap()
{
    if (xkbDefaultKeymap_ || !xkbCtx_) {
        return;
    }
    // Empty rule_names → XKB resolves to XKB_DEFAULT_LAYOUT (typically "us").
    // Used solely for baseLayoutKeyCodepoint (kitty `base_layout_key`).
    // Failure is non-fatal — callers return 0, which the encoder elides.
    xkb_rule_names empty = { };
    xkbDefaultKeymap_    = xkb_keymap_new_from_names(xkbCtx_, &empty, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (xkbDefaultKeymap_) {
        xkbDefaultState_ = xkb_state_new(xkbDefaultKeymap_);
        if (!xkbDefaultState_) {
            xkb_keymap_unref(xkbDefaultKeymap_);
            xkbDefaultKeymap_ = nullptr;
        }
    }
}

// ---------- lifecycle ----------

bool WaylandWindow::create(int width, int height, const std::string &title)
{
    defaultWidth_  = width > 0 ? width : 800;
    defaultHeight_ = height > 0 ? height : 600;

    xkbCtx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkbCtx_) {
        spdlog::error("WaylandWindow: xkb_context_new failed");
        return false;
    }

    display_ = wl_display_connect(nullptr);
    if (!display_) {
        spdlog::error("WaylandWindow: wl_display_connect failed (WAYLAND_DISPLAY={})",
                      std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "<unset>");
        destroy();
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
    cancelKeyRepeat();

    if (wlFd_ >= 0) {
        loop_.removeFd(wlFd_);
        wlFd_ = -1;
    }
    if (keyboard_) {
        wl_keyboard_release(keyboard_);
        keyboard_ = nullptr;
    }
    // Destroy in reverse construction order; xdg_wm_base requires all of
    // its xdg_surfaces to be gone first. Registry-bound globals
    // (compositor, wmBase, seat) and the registry itself are released by
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
    seat_       = nullptr;
    wmBase_     = nullptr;
    compositor_ = nullptr;
    registry_   = nullptr;
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }

    if (xkbState_) {
        xkb_state_unref(xkbState_);
        xkbState_ = nullptr;
    }
    if (xkbCleanState_) {
        xkb_state_unref(xkbCleanState_);
        xkbCleanState_ = nullptr;
    }
    if (xkbKeymap_) {
        xkb_keymap_unref(xkbKeymap_);
        xkbKeymap_ = nullptr;
    }
    if (xkbDefaultState_) {
        xkb_state_unref(xkbDefaultState_);
        xkbDefaultState_ = nullptr;
    }
    if (xkbDefaultKeymap_) {
        xkb_keymap_unref(xkbDefaultKeymap_);
        xkbDefaultKeymap_ = nullptr;
    }
    if (xkbCtx_) {
        xkb_context_unref(xkbCtx_);
        xkbCtx_ = nullptr;
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

std::string WaylandWindow::keyName(int keycode) const
{
    return mb::xkb::keyName(xkbState_, keycode);
}

uint32_t WaylandWindow::shiftedKeyCodepoint(int keycode) const
{
    return mb::xkb::shiftedKeyCodepoint(xkbState_, xkbKeymap_, keycode);
}

uint32_t WaylandWindow::baseLayoutKeyCodepoint(int keycode) const
{
    // Default-rules keymap is built lazily so a keyless Wayland session
    // (no wl_keyboard.keymap delivered) still doesn't crash here.
    const_cast<WaylandWindow *>(this)->ensureDefaultKeymap();
    return mb::xkb::baseLayoutKeyCodepoint(xkbDefaultKeymap_, keycode);
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
