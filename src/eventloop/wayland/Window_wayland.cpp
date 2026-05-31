#include "Window_wayland.h"

#include <InputTypes.h>
#include <xkb/Xkb.h>

#include <dawn/webgpu_cpp.h>
#include <spdlog/spdlog.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
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
// wl_compositor v6 added wl_surface.preferred_buffer_scale + .preferred_buffer_transform —
// the integer-scale fallback used when wp_fractional_scale_v1 isn't available.
constexpr uint32_t kCompositorClientVersion     = 6;
constexpr uint32_t kXdgWmBaseClientVersion      = 5;
// wl_seat v5 added `name`; v4 added `wl_keyboard.repeat_info`. v7 is the
// current upstream version and adds `wl_pointer.frame` etc. Bind v7 if the
// compositor advertises it so Stage 3 (pointer) doesn't need a re-bind.
constexpr uint32_t kSeatClientVersion           = 7;
constexpr uint32_t kCursorShapeMgrClientVersion = 2;
constexpr uint32_t kDataDeviceMgrClientVersion  = 3;
constexpr uint32_t kPrimarySelMgrClientVersion  = 1;
constexpr uint32_t kViewporterClientVersion     = 1;
constexpr uint32_t kFractionalScaleMgrVersion   = 1;
constexpr uint32_t kActivationClientVersion     = 1;
// wl_output v2 adds `scale`, v3 adds `release`, v4 adds `name`/`description`.
// Anything ≥ v2 is sufficient for getScreenSize.
constexpr uint32_t kOutputClientVersion         = 4;

// fractional-scale-v1 reports scale as an integer = scale × 120 (so 240 = 2.0).
// Convert to float on the fly.
constexpr int kFractionalScaleUnit = 120;

// MIME types we offer when we own a selection, and the priority order we
// pick from when consuming someone else's. Listed best-first.
constexpr const char *kMimeUtf8      = "text/plain;charset=utf-8";
constexpr const char *kMimePlain     = "text/plain";
constexpr const char *kMimeUtf8X11   = "UTF8_STRING";
constexpr const char *kMimeStringX11 = "STRING";

bool isAcceptableTextMime(const std::string &m)
{
    return m == kMimeUtf8 || m == kMimePlain || m == kMimeUtf8X11 || m == kMimeStringX11;
}

const char *pickBestMime(const std::vector<std::string> &mimes)
{
    const char *order[] = { kMimeUtf8, kMimeUtf8X11, kMimePlain, kMimeStringX11 };
    for (const char *want : order) {
        for (const auto &m : mimes) {
            if (m == want) {
                return want;
            }
        }
    }
    return nullptr;
}

constexpr uint64_t kSelectionReadTimeoutMs = 5000;

// Cursor-shape-v1 enum mapping for our 12 CursorStyle values.
uint32_t cursorStyleToShape(Window::CursorStyle s)
{
    switch (s) {
        case Window::CursorStyle::Arrow: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
        case Window::CursorStyle::IBeam: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        case Window::CursorStyle::Pointer: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
        case Window::CursorStyle::Crosshair: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
        case Window::CursorStyle::Wait: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
        case Window::CursorStyle::Help: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
        case Window::CursorStyle::Move: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
        case Window::CursorStyle::NotAllowed: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
        case Window::CursorStyle::ResizeH: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
        case Window::CursorStyle::ResizeV: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
        case Window::CursorStyle::ResizeNESW: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
        case Window::CursorStyle::ResizeNWSE: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    }
    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

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

const wl_pointer_listener WaylandWindow::kPointerListener = {
    .enter         = &WaylandWindow::onPointerEnter,
    .leave         = &WaylandWindow::onPointerLeave,
    .motion        = &WaylandWindow::onPointerMotion,
    .button        = &WaylandWindow::onPointerButton,
    .axis          = &WaylandWindow::onPointerAxis,
    .frame         = &WaylandWindow::onPointerFrame,
    .axis_source   = &WaylandWindow::onPointerAxisSource,
    .axis_stop     = &WaylandWindow::onPointerAxisStop,
    .axis_discrete = &WaylandWindow::onPointerAxisDiscrete,
};

const wl_data_device_listener WaylandWindow::kDataDeviceListener = {
    .data_offer = &WaylandWindow::onDataDeviceDataOffer,
    .enter      = &WaylandWindow::onDataDeviceEnter,
    .leave      = &WaylandWindow::onDataDeviceLeave,
    .motion     = &WaylandWindow::onDataDeviceMotion,
    .drop       = &WaylandWindow::onDataDeviceDrop,
    .selection  = &WaylandWindow::onDataDeviceSelection,
};

const wl_data_source_listener WaylandWindow::kDataSourceListener = {
    .target             = &WaylandWindow::onDataSourceTarget,
    .send               = &WaylandWindow::onDataSourceSend,
    .cancelled          = &WaylandWindow::onDataSourceCancelled,
    .dnd_drop_performed = &WaylandWindow::onDataSourceDndDropPerformed,
    .dnd_finished       = &WaylandWindow::onDataSourceDndFinished,
    .action             = &WaylandWindow::onDataSourceAction,
};

const wl_data_offer_listener WaylandWindow::kDataOfferListener = {
    .offer          = &WaylandWindow::onDataOfferOffer,
    .source_actions = &WaylandWindow::onDataOfferSourceActions,
    .action         = &WaylandWindow::onDataOfferAction,
};

const zwp_primary_selection_device_v1_listener WaylandWindow::kPrimaryDeviceListener = {
    .data_offer = &WaylandWindow::onPrimaryDeviceDataOffer,
    .selection  = &WaylandWindow::onPrimaryDeviceSelection,
};

const zwp_primary_selection_source_v1_listener WaylandWindow::kPrimarySourceListener = {
    .send      = &WaylandWindow::onPrimarySourceSend,
    .cancelled = &WaylandWindow::onPrimarySourceCancelled,
};

const zwp_primary_selection_offer_v1_listener WaylandWindow::kPrimaryOfferListener = {
    .offer = &WaylandWindow::onPrimaryOfferOffer,
};

const wl_surface_listener WaylandWindow::kSurfaceListener = {
    .enter                      = &WaylandWindow::onSurfaceEnter,
    .leave                      = &WaylandWindow::onSurfaceLeave,
    .preferred_buffer_scale     = &WaylandWindow::onSurfacePreferredBufferScale,
    .preferred_buffer_transform = &WaylandWindow::onSurfacePreferredBufferTransform,
};

const wl_output_listener WaylandWindow::kOutputListener = {
    .geometry    = &WaylandWindow::onOutputGeometry,
    .mode        = &WaylandWindow::onOutputMode,
    .done        = &WaylandWindow::onOutputDone,
    .scale       = &WaylandWindow::onOutputScale,
    .name        = &WaylandWindow::onOutputName,
    .description = &WaylandWindow::onOutputDescription,
};

const wp_fractional_scale_v1_listener WaylandWindow::kFractionalScaleListener = {
    .preferred_scale = &WaylandWindow::onFractionalScalePreferredScale,
};

const xdg_activation_token_v1_listener WaylandWindow::kActivationTokenListener = {
    .done = &WaylandWindow::onActivationTokenDone,
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
        // If the data-device / primary-selection managers have already
        // arrived, hook the per-seat devices up now.
        if ((self->dataDeviceMgr_ && !self->dataDevice_) || (self->primarySelMgr_ && !self->primarySelDevice_)) {
            self->attachSeatSelections();
        }
    } else if (std::strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        uint32_t v            = std::min(version, kCursorShapeMgrClientVersion);
        self->cursorShapeMgr_ = static_cast<wp_cursor_shape_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, v));
        // Registry-global delivery order is unspecified: if the seat
        // already attached a pointer before this global arrived, create
        // the cursor-shape device now and re-apply the current style.
        if (self->pointer_ && !self->cursorShapeDev_) {
            self->cursorShapeDev_ = wp_cursor_shape_manager_v1_get_pointer(self->cursorShapeMgr_, self->pointer_);
            self->applyCursorShape();
        }
    } else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        uint32_t v           = std::min(version, kDataDeviceMgrClientVersion);
        self->dataDeviceMgr_ = static_cast<wl_data_device_manager *>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, v));
        // Same delivery-order-unspecified caveat as cursor-shape: if the
        // seat was already bound, attach the per-seat device now.
        if (self->seat_ && !self->dataDevice_) {
            self->attachSeatSelections();
        }
    } else if (std::strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        uint32_t v           = std::min(version, kPrimarySelMgrClientVersion);
        self->primarySelMgr_ = static_cast<zwp_primary_selection_device_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, v));
        if (self->seat_ && !self->primarySelDevice_) {
            self->attachSeatSelections();
        }
    } else if (std::strcmp(iface, wp_viewporter_interface.name) == 0) {
        uint32_t v        = std::min(version, kViewporterClientVersion);
        self->viewporter_ = static_cast<wp_viewporter *>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, v));
    } else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        uint32_t v           = std::min(version, kFractionalScaleMgrVersion);
        self->fractionalMgr_ = static_cast<wp_fractional_scale_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, v));
    } else if (std::strcmp(iface, xdg_activation_v1_interface.name) == 0) {
        uint32_t v        = std::min(version, kActivationClientVersion);
        self->activation_ = static_cast<xdg_activation_v1 *>(
            wl_registry_bind(registry, name, &xdg_activation_v1_interface, v));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        uint32_t v   = std::min(version, kOutputClientVersion);
        auto *output = static_cast<wl_output *>(
            wl_registry_bind(registry, name, &wl_output_interface, v));
        OutputInfo info;
        info.output = output;
        info.name   = name;
        self->outputs_.push_back(info);
        wl_output_add_listener(output, &kOutputListener, self);
    }
}

void WaylandWindow::onRegistryGlobalRemove(void *data, wl_registry *, uint32_t name)
{
    // Outputs are the only global we currently expect to come and go at
    // runtime (monitor hotplug). Other globals (seat, compositor, wmBase,
    // activation, viewporter, fractional-scale, data-device-manager,
    // primary-selection-manager, cursor-shape-manager) are typically
    // session-lifetime — if one of those does go away we leave the
    // matching pointer non-null and accept the eventual protocol error,
    // matching the behaviour of every other Wayland client in this niche.
    auto *self = static_cast<WaylandWindow *>(data);
    for (auto it = self->outputs_.begin(); it != self->outputs_.end(); ++it) {
        if (it->name == name) {
            wl_output_destroy(it->output);
            self->outputs_.erase(it);
            return;
        }
    }
}

void WaylandWindow::onXdgWmBasePing(void *, xdg_wm_base *base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}

void WaylandWindow::onXdgSurfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
{
    auto *self = static_cast<WaylandWindow *>(data);
    xdg_surface_ack_configure(surface, serial);

    // Resolve pending LOGICAL size: 0 from toplevel.configure means
    // "client picks". First configure with 0/0 falls back to the
    // create()-requested size; subsequent 0/0 configures keep the existing
    // logical size.
    int newW = self->pendingLogicalW_;
    int newH = self->pendingLogicalH_;
    if (newW <= 0) {
        newW = self->firstConfigure_ ? self->logicalW_ : self->defaultWidth_;
    }
    if (newH <= 0) {
        newH = self->firstConfigure_ ? self->logicalH_ : self->defaultHeight_;
    }

    self->logicalW_ = newW;
    self->logicalH_ = newH;

    if (!self->firstConfigure_) {
        // First configure: create() is blocked in wl_display_roundtrip and
        // will read width_/height_ after the roundtrip returns. applyScale
        // computes width_/height_ from logical × scale without firing
        // callbacks yet — PlatformDawn reads via getFramebufferSize.
        self->firstConfigure_ = true;
        self->applyScale();
    } else {
        // Subsequent configure: applyScale recomputes physical, updates
        // viewport / set_buffer_scale, and fires onFramebufferResize if
        // the physical size changed.
        self->applyScale();
    }
}

void WaylandWindow::onXdgToplevelConfigure(void *data, xdg_toplevel *, int32_t width, int32_t height, struct wl_array *states)
{
    auto *self             = static_cast<WaylandWindow *>(data);
    self->pendingLogicalW_ = width;
    self->pendingLogicalH_ = height;
    self->parseToplevelStates(states);
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

    const bool hasPtr = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (hasPtr && !self->pointer_) {
        self->attachPointer(wl_seat_get_pointer(seat));
    } else if (!hasPtr && self->pointer_) {
        self->releasePointer();
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

void WaylandWindow::onKeyboardEnter(void *data, wl_keyboard *, uint32_t serial, wl_surface *, struct wl_array *)
{
    auto *self             = static_cast<WaylandWindow *>(data);
    self->lastInputSerial_ = serial;
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

void WaylandWindow::onKeyboardKey(void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
{
    auto *self             = static_cast<WaylandWindow *>(data);
    self->lastInputSerial_ = serial;
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
            onChar(cp, unshifted, static_cast<int>(xkbKeycode));
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

// ---------- pointer ----------

void WaylandWindow::attachPointer(wl_pointer *pointer)
{
    pointer_ = pointer;
    if (!pointer_) {
        return;
    }
    wl_pointer_add_listener(pointer_, &kPointerListener, this);
    if (cursorShapeMgr_) {
        cursorShapeDev_ = wp_cursor_shape_manager_v1_get_pointer(cursorShapeMgr_, pointer_);
    }
}

void WaylandWindow::releasePointer()
{
    hasPointerFocus_ = false;
    if (cursorShapeDev_) {
        wp_cursor_shape_device_v1_destroy(cursorShapeDev_);
        cursorShapeDev_ = nullptr;
    }
    if (pointer_) {
        wl_pointer_release(pointer_);
        pointer_ = nullptr;
    }
}

void WaylandWindow::applyCursorShape()
{
    if (!cursorShapeDev_ || !hasPointerFocus_ || pointerEnterSerial_ == 0) {
        return;
    }
    wp_cursor_shape_device_v1_set_shape(cursorShapeDev_, pointerEnterSerial_, cursorStyleToShape(currentCursor_));
}

void WaylandWindow::onPointerEnter(void *data, wl_pointer *, uint32_t serial, wl_surface *surface, int32_t x, int32_t y)
{
    auto *self = static_cast<WaylandWindow *>(data);
    // Multi-surface clients would gate on `surface == self->surface_`; we
    // only ever own one wl_surface, but check anyway to be defensive.
    if (surface != self->surface_) {
        return;
    }
    self->hasPointerFocus_    = true;
    self->pointerEnterSerial_ = serial;
    self->lastInputSerial_    = serial;
    // Always (re-)apply the cursor on enter — the compositor may have
    // reset to the default between leave/enter.
    self->applyCursorShape();
    if (self->onCursorPos) {
        self->onCursorPos(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
}

void WaylandWindow::onPointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *surface)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (surface != self->surface_) {
        return;
    }
    self->hasPointerFocus_ = false;
}

void WaylandWindow::onPointerMotion(void *data, wl_pointer *, uint32_t, int32_t x, int32_t y)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (self->onCursorPos) {
        self->onCursorPos(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
}

void WaylandWindow::onPointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
    auto *self             = static_cast<WaylandWindow *>(data);
    self->lastInputSerial_ = serial;
    int btn                = -1;
    switch (button) {
        case BTN_LEFT: btn = static_cast<int>(LeftButton); break;
        case BTN_MIDDLE: btn = static_cast<int>(MidButton); break;
        case BTN_RIGHT: btn = static_cast<int>(RightButton); break;
        default: return; // ignore BTN_SIDE / BTN_EXTRA / etc. for now
    }
    uint32_t mods = self->xkbState_ ? mb::xkb::stateToModifiers(self->xkbState_) : 0u;
    KeyAction act = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? KeyAction_Press : KeyAction_Release;
    if (self->onMouseButton) {
        self->onMouseButton(btn, static_cast<int>(act), static_cast<int>(mods));
    }
}

void WaylandWindow::onPointerAxis(void *data, wl_pointer *, uint32_t, uint32_t axis, int32_t value)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (!self->onScroll) {
        return;
    }
    // Wayland's axis value is in surface coordinates; one notch on a wheel
    // is typically ±10. Divide by 10 so onScroll receives ~±1 per notch,
    // matching the XCB backend (which fires onScroll(0, ±1) per wheel
    // button press). Touchpad two-finger scroll generates smaller values
    // that pass through as fractional ticks — the scrollback/wheel
    // handlers in PlatformDawn only check sign, so any non-zero works.
    //
    // Sign convention: Wayland positive vertical = scroll content down
    // (wheel away from user). XCB onScroll dy>0 = scroll up (wheel toward
    // user). Negate so they agree.
    const double v = wl_fixed_to_double(value) / 10.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        self->onScroll(0.0, -v);
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        self->onScroll(v, 0.0);
    }
}

void WaylandWindow::onPointerFrame(void *, wl_pointer *)
{
    // v5+ event group terminator. Stage 3 dispatches each event on receipt
    // (no inter-event coalescing), so frame is a no-op.
}

void WaylandWindow::onPointerAxisSource(void *, wl_pointer *, uint32_t)
{
    // Wheel vs continuous vs finger source classification. Unused.
}

void WaylandWindow::onPointerAxisStop(void *, wl_pointer *, uint32_t, uint32_t)
{
    // End-of-touchpad-scroll marker. Unused.
}

void WaylandWindow::onPointerAxisDiscrete(void *, wl_pointer *, uint32_t, int32_t)
{
    // Per-frame discrete tick count for axis events. Unused — our onScroll
    // consumes the continuous axis value with the /10 normalization above.
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

// ---------- selection (clipboard + primary) ----------

void WaylandWindow::attachSeatSelections()
{
    if (!seat_) {
        return;
    }
    if (dataDeviceMgr_ && !dataDevice_) {
        dataDevice_ = wl_data_device_manager_get_data_device(dataDeviceMgr_, seat_);
        if (dataDevice_) {
            wl_data_device_add_listener(dataDevice_, &kDataDeviceListener, this);
        }
    }
    if (primarySelMgr_ && !primarySelDevice_) {
        primarySelDevice_ = zwp_primary_selection_device_manager_v1_get_device(primarySelMgr_, seat_);
        if (primarySelDevice_) {
            zwp_primary_selection_device_v1_add_listener(primarySelDevice_, &kPrimaryDeviceListener, this);
        }
    }
}

void WaylandWindow::releaseSeatSelections()
{
    resetClipboardSource();
    resetPrimarySource();
    if (currentClipboardOffer_) {
        dataOfferMimes_.erase(currentClipboardOffer_);
        wl_data_offer_destroy(currentClipboardOffer_);
        currentClipboardOffer_ = nullptr;
    }
    if (currentPrimaryOffer_) {
        primaryOfferMimes_.erase(currentPrimaryOffer_);
        zwp_primary_selection_offer_v1_destroy(currentPrimaryOffer_);
        currentPrimaryOffer_ = nullptr;
    }
    if (dataDevice_) {
        // wl_data_device.release was added in wl_data_device_manager v2; we
        // always bind ≥v3. Falls back to destroy on older but the cast
        // would still succeed.
        wl_data_device_release(dataDevice_);
        dataDevice_ = nullptr;
    }
    if (primarySelDevice_) {
        zwp_primary_selection_device_v1_destroy(primarySelDevice_);
        primarySelDevice_ = nullptr;
    }
}

void WaylandWindow::resetClipboardSource()
{
    if (clipboardSource_) {
        wl_data_source_destroy(clipboardSource_);
        clipboardSource_ = nullptr;
    }
}

void WaylandWindow::resetPrimarySource()
{
    if (primarySource_) {
        zwp_primary_selection_source_v1_destroy(primarySource_);
        primarySource_ = nullptr;
    }
}

void WaylandWindow::writeStringToFd(int fd, const std::string &text)
{
    // The peer's pipe write end is non-blocking — short writes happen for
    // small buffers and signal-interrupts. Loop until done or hard fail.
    // Always close the fd regardless: leaking it would block the peer's
    // read with no EOF and prevent another set_selection from succeeding.
    const char *p = text.data();
    size_t left   = text.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n > 0) {
            p += static_cast<size_t>(n);
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN here means the peer is slow; we still need to make
        // progress without blocking the main thread. Yield via a short
        // poll on writability would be cleaner, but for the small amounts
        // typical of terminal selections (≤ a few KB) the kernel buffer
        // accommodates it; the rare case where it doesn't drops the tail.
        // Log + break.
        spdlog::warn("WaylandWindow: selection write failed/short: {}", std::strerror(errno));
        break;
    }
    close(fd);
}

void WaylandWindow::startSelectionRead(int readFd, SelectionCallback cb)
{
    auto entry = std::make_unique<PendingSelectionRead>();
    entry->fd  = readFd;
    entry->cb  = std::move(cb);
    auto *raw  = entry.get();

    // Non-blocking + close-on-exec hygiene for the read end. pipe2(O_CLOEXEC)
    // is set at create time; flip O_NONBLOCK so the readable callback never
    // blocks if the kernel returns early.
    int flags = fcntl(readFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(readFd, F_SETFL, flags | O_NONBLOCK);
    }

    loop_.watchFd(readFd, EventLoop::FdEvents::Readable, [this, raw](EventLoop::FdEvents)
                  {
                      // Drain whatever's available; on EOF (0) or hard error,
                      // complete the read and pop the entry. EAGAIN means
                      // "wait for next wake."
                      char chunk[4096];
                      for (;;) {
                          ssize_t n = read(raw->fd, chunk, sizeof(chunk));
                          if (n > 0) {
                              raw->buf.insert(raw->buf.end(), chunk, chunk + n);
                              continue;
                          }
                          if (n < 0 && errno == EINTR) {
                              continue;
                          }
                          if (n < 0 && errno == EAGAIN) {
                              return;
                          }
                          // EOF or hard error → complete.
                          std::optional<std::string> result;
                          if (n == 0 && !raw->buf.empty()) {
                              result = std::string(raw->buf.data(), raw->buf.size());
                          } else if (n == 0) {
                              result = std::string { };
                          }
                          SelectionCallback cb = std::move(raw->cb);
                          loop_.removeFd(raw->fd);
                          if (raw->timeoutTimer) {
                              loop_.removeTimer(raw->timeoutTimer);
                          }
                          close(raw->fd);
                          // Erase the entry (raw is invalidated after this).
                          for (auto it = pendingReads_.begin(); it != pendingReads_.end(); ++it) {
                              if (it->get() == raw) {
                                  pendingReads_.erase(it);
                                  break;
                              }
                          }
                          if (cb) {
                              cb(result);
                          }
                          return;
                      }
                  });

    entry->timeoutTimer = loop_.addTimer(kSelectionReadTimeoutMs, false, [this, raw]()
                                         {
                                             // Timeout: cancel the read, fire cb with nullopt.
                                             SelectionCallback cb = std::move(raw->cb);
                                             loop_.removeFd(raw->fd);
                                             close(raw->fd);
                                             raw->timeoutTimer = 0;
                                             for (auto it = pendingReads_.begin(); it != pendingReads_.end(); ++it) {
                                                 if (it->get() == raw) {
                                                     pendingReads_.erase(it);
                                                     break;
                                                 }
                                             }
                                             if (cb) {
                                                 cb(std::nullopt);
                                             }
                                         });

    pendingReads_.push_back(std::move(entry));
}

void WaylandWindow::cancelAllSelectionReads()
{
    for (auto &p : pendingReads_) {
        if (p->fd >= 0) {
            loop_.removeFd(p->fd);
            close(p->fd);
        }
        if (p->timeoutTimer) {
            loop_.removeTimer(p->timeoutTimer);
        }
    }
    pendingReads_.clear();
}

// Clipboard public API

void WaylandWindow::setClipboard(const std::string &text)
{
    if (!dataDeviceMgr_ || !dataDevice_) {
        // Without the manager + per-seat device we can't own the
        // selection; nothing to do.
        clipboardContent_ = text;
        return;
    }
    clipboardContent_ = text;
    resetClipboardSource();
    clipboardSource_ = wl_data_device_manager_create_data_source(dataDeviceMgr_);
    if (!clipboardSource_) {
        return;
    }
    wl_data_source_add_listener(clipboardSource_, &kDataSourceListener, this);
    wl_data_source_offer(clipboardSource_, kMimeUtf8);
    wl_data_source_offer(clipboardSource_, kMimePlain);
    wl_data_source_offer(clipboardSource_, kMimeUtf8X11);
    wl_data_source_offer(clipboardSource_, kMimeStringX11);
    wl_data_device_set_selection(dataDevice_, clipboardSource_, lastInputSerial_);
    if (display_) {
        wl_display_flush(display_);
    }
}

void WaylandWindow::setPrimarySelection(const std::string &text)
{
    if (!primarySelMgr_ || !primarySelDevice_) {
        primaryContent_ = text;
        return;
    }
    primaryContent_ = text;
    resetPrimarySource();
    primarySource_ = zwp_primary_selection_device_manager_v1_create_source(primarySelMgr_);
    if (!primarySource_) {
        return;
    }
    zwp_primary_selection_source_v1_add_listener(primarySource_, &kPrimarySourceListener, this);
    zwp_primary_selection_source_v1_offer(primarySource_, kMimeUtf8);
    zwp_primary_selection_source_v1_offer(primarySource_, kMimePlain);
    zwp_primary_selection_source_v1_offer(primarySource_, kMimeUtf8X11);
    zwp_primary_selection_source_v1_offer(primarySource_, kMimeStringX11);
    zwp_primary_selection_device_v1_set_selection(primarySelDevice_, primarySource_, lastInputSerial_);
    if (display_) {
        wl_display_flush(display_);
    }
}

void WaylandWindow::requestSelection(SelectionSource src, SelectionCallback cb)
{
    if (!cb) {
        return;
    }

    // Self-loop fast path: if we own the matching selection, satisfy from
    // our cached content directly. Avoids the pipe round-trip and works
    // even on compositors that don't deliver `selection` to the owner.
    if (src == SelectionSource::Clipboard && clipboardSource_) {
        cb(clipboardContent_.empty() ? std::optional<std::string> { } : clipboardContent_);
        return;
    }
    if (src == SelectionSource::Primary && primarySource_) {
        cb(primaryContent_.empty() ? std::optional<std::string> { } : primaryContent_);
        return;
    }

    if (src == SelectionSource::Clipboard) {
        if (!currentClipboardOffer_) {
            cb(std::nullopt);
            return;
        }
        auto it          = dataOfferMimes_.find(currentClipboardOffer_);
        const char *mime = (it != dataOfferMimes_.end()) ? pickBestMime(it->second.mimes) : nullptr;
        if (!mime) {
            cb(std::nullopt);
            return;
        }
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            spdlog::error("WaylandWindow: pipe2 failed for clipboard read: {}", std::strerror(errno));
            cb(std::nullopt);
            return;
        }
        wl_data_offer_receive(currentClipboardOffer_, mime, fds[1]);
        close(fds[1]);
        if (display_) {
            wl_display_flush(display_);
        }
        startSelectionRead(fds[0], std::move(cb));
    } else {
        if (!currentPrimaryOffer_) {
            cb(std::nullopt);
            return;
        }
        auto it          = primaryOfferMimes_.find(currentPrimaryOffer_);
        const char *mime = (it != primaryOfferMimes_.end()) ? pickBestMime(it->second.mimes) : nullptr;
        if (!mime) {
            cb(std::nullopt);
            return;
        }
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            spdlog::error("WaylandWindow: pipe2 failed for primary read: {}", std::strerror(errno));
            cb(std::nullopt);
            return;
        }
        zwp_primary_selection_offer_v1_receive(currentPrimaryOffer_, mime, fds[1]);
        close(fds[1]);
        if (display_) {
            wl_display_flush(display_);
        }
        startSelectionRead(fds[0], std::move(cb));
    }
}

// Data device / offer / source listener bodies

void WaylandWindow::onDataDeviceDataOffer(void *data, wl_data_device *, wl_data_offer *offer)
{
    auto *self                   = static_cast<WaylandWindow *>(data);
    // Attach a listener so the per-offer `offer` events populate our mime
    // map. We track the offer in dataOfferMimes_ keyed by pointer.
    self->dataOfferMimes_[offer] = OfferMimes { };
    wl_data_offer_add_listener(offer, &kDataOfferListener, self);
}

void WaylandWindow::onDataDeviceEnter(void *, wl_data_device *, uint32_t, wl_surface *, int32_t, int32_t, wl_data_offer *)
{
    // Drag-and-drop entry. Out of scope for Stage 4 (no DnD).
}

void WaylandWindow::onDataDeviceLeave(void *, wl_data_device *)
{
    // DnD leave. Out of scope.
}

void WaylandWindow::onDataDeviceMotion(void *, wl_data_device *, uint32_t, int32_t, int32_t)
{
    // DnD motion. Out of scope.
}

void WaylandWindow::onDataDeviceDrop(void *, wl_data_device *)
{
    // DnD drop. Out of scope.
}

void WaylandWindow::onDataDeviceSelection(void *data, wl_data_device *, wl_data_offer *offer)
{
    auto *self = static_cast<WaylandWindow *>(data);
    // The previous offer (if any) is being superseded; drop our tracking +
    // destroy it. Note: when WE own the selection (clipboardSource_ set),
    // the compositor may pass `offer == null` as a no-op, or our own
    // proxy back — either way replace.
    if (self->currentClipboardOffer_ && self->currentClipboardOffer_ != offer) {
        self->dataOfferMimes_.erase(self->currentClipboardOffer_);
        wl_data_offer_destroy(self->currentClipboardOffer_);
    }
    self->currentClipboardOffer_ = offer;
}

void WaylandWindow::onDataOfferOffer(void *data, wl_data_offer *offer, const char *mime)
{
    auto *self = static_cast<WaylandWindow *>(data);
    auto it    = self->dataOfferMimes_.find(offer);
    if (it == self->dataOfferMimes_.end() || !mime) {
        return;
    }
    if (isAcceptableTextMime(mime)) {
        it->second.mimes.emplace_back(mime);
    }
}

void WaylandWindow::onDataOfferSourceActions(void *, wl_data_offer *, uint32_t)
{
    // DnD action bitmask. Out of scope for Stage 4.
}

void WaylandWindow::onDataOfferAction(void *, wl_data_offer *, uint32_t)
{
    // DnD active action. Out of scope.
}

void WaylandWindow::onDataSourceTarget(void *, wl_data_source *, const char *)
{
    // DnD target ack. Out of scope.
}

void WaylandWindow::onDataSourceSend(void *data, wl_data_source *source, const char *mime, int32_t fd)
{
    auto *self = static_cast<WaylandWindow *>(data);
    (void)mime; // we only offer text MIMEs; any choice gets the same bytes
    if (source != self->clipboardSource_) {
        // Stale source (we already moved on). Close the fd so the peer
        // unblocks rather than waiting for an EOF that never comes.
        close(fd);
        return;
    }
    self->writeStringToFd(fd, self->clipboardContent_);
}

void WaylandWindow::onDataSourceCancelled(void *data, wl_data_source *source)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (source != self->clipboardSource_) {
        return;
    }
    // Another client took the selection. Drop our source + content.
    self->resetClipboardSource();
    self->clipboardContent_.clear();
}

void WaylandWindow::onDataSourceDndDropPerformed(void *, wl_data_source *)
{
    // DnD-only. Out of scope.
}

void WaylandWindow::onDataSourceDndFinished(void *, wl_data_source *)
{
    // DnD-only. Out of scope.
}

void WaylandWindow::onDataSourceAction(void *, wl_data_source *, uint32_t)
{
    // DnD-only. Out of scope.
}

void WaylandWindow::onPrimaryDeviceDataOffer(void *data, zwp_primary_selection_device_v1 *, zwp_primary_selection_offer_v1 *offer)
{
    auto *self                      = static_cast<WaylandWindow *>(data);
    self->primaryOfferMimes_[offer] = OfferMimes { };
    zwp_primary_selection_offer_v1_add_listener(offer, &kPrimaryOfferListener, self);
}

void WaylandWindow::onPrimaryDeviceSelection(void *data, zwp_primary_selection_device_v1 *, zwp_primary_selection_offer_v1 *offer)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (self->currentPrimaryOffer_ && self->currentPrimaryOffer_ != offer) {
        self->primaryOfferMimes_.erase(self->currentPrimaryOffer_);
        zwp_primary_selection_offer_v1_destroy(self->currentPrimaryOffer_);
    }
    self->currentPrimaryOffer_ = offer;
}

void WaylandWindow::onPrimaryOfferOffer(void *data, zwp_primary_selection_offer_v1 *offer, const char *mime)
{
    auto *self = static_cast<WaylandWindow *>(data);
    auto it    = self->primaryOfferMimes_.find(offer);
    if (it == self->primaryOfferMimes_.end() || !mime) {
        return;
    }
    if (isAcceptableTextMime(mime)) {
        it->second.mimes.emplace_back(mime);
    }
}

void WaylandWindow::onPrimarySourceSend(void *data, zwp_primary_selection_source_v1 *source, const char *mime, int32_t fd)
{
    auto *self = static_cast<WaylandWindow *>(data);
    (void)mime;
    if (source != self->primarySource_) {
        close(fd);
        return;
    }
    self->writeStringToFd(fd, self->primaryContent_);
}

void WaylandWindow::onPrimarySourceCancelled(void *data, zwp_primary_selection_source_v1 *source)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (source != self->primarySource_) {
        return;
    }
    self->resetPrimarySource();
    self->primaryContent_.clear();
}

// ---------- HiDPI / fractional scale ----------

void WaylandWindow::onSurfaceEnter(void *data, wl_surface *, struct wl_output *output)
{
    // Rendering scale comes from preferred_buffer_scale / wp_fractional_scale_v1,
    // not from which output we entered. The enter/leave events are still
    // useful for getScreenSize: we mark which outputs the surface is on so
    // getScreenSize can prefer them.
    auto *self = static_cast<WaylandWindow *>(data);
    for (auto &info : self->outputs_) {
        if (info.output == output) {
            info.entered = true;
            return;
        }
    }
}

void WaylandWindow::onSurfaceLeave(void *data, wl_surface *, struct wl_output *output)
{
    auto *self = static_cast<WaylandWindow *>(data);
    for (auto &info : self->outputs_) {
        if (info.output == output) {
            info.entered = false;
            return;
        }
    }
}

void WaylandWindow::onSurfacePreferredBufferScale(void *data, wl_surface *, int32_t factor)
{
    auto *self          = static_cast<WaylandWindow *>(data);
    self->integerScale_ = factor > 0 ? factor : 1;
    self->applyScale();
}

void WaylandWindow::onSurfacePreferredBufferTransform(void *, wl_surface *, uint32_t)
{
    // Terminals don't render at non-identity transforms; we ignore the
    // hint and let the compositor apply the rotation server-side.
}

void WaylandWindow::onFractionalScalePreferredScale(void *data, wp_fractional_scale_v1 *, uint32_t scale120ths)
{
    auto *self                 = static_cast<WaylandWindow *>(data);
    self->haveFractionalScale_ = true;
    self->fractionalScale120_  = static_cast<int>(scale120ths);
    self->applyScale();
}

// ---------- wl_output (current monitor info) ----------

void WaylandWindow::onOutputGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t)
{
    // physical_width / physical_height (mm) + transform are unused; the
    // mode event supplies pixel dimensions.
}

void WaylandWindow::onOutputMode(void *data, wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t)
{
    // Only the WL_OUTPUT_MODE_CURRENT bit is relevant. Other modes
    // (preferred / available) describe what the monitor could do.
    constexpr uint32_t kCurrent = 0x1; // WL_OUTPUT_MODE_CURRENT
    if (!(flags & kCurrent)) {
        return;
    }
    auto *self = static_cast<WaylandWindow *>(data);
    for (auto &info : self->outputs_) {
        if (info.output == output) {
            info.width  = width;
            info.height = height;
            return;
        }
    }
}

void WaylandWindow::onOutputScale(void *data, wl_output *output, int32_t factor)
{
    auto *self = static_cast<WaylandWindow *>(data);
    for (auto &info : self->outputs_) {
        if (info.output == output) {
            info.scale = factor > 0 ? factor : 1;
            return;
        }
    }
}

void WaylandWindow::onOutputDone(void *, wl_output *)
{
    // `done` marks the end of a batch of property updates. Nothing else
    // to commit — getScreenSize reads the latest values directly.
}

void WaylandWindow::onOutputName(void *, wl_output *, const char *)
{
    // Logical-name string (v4+); unused.
}

void WaylandWindow::onOutputDescription(void *, wl_output *, const char *)
{
    // Human-readable description (v4+); unused.
}

void WaylandWindow::applyScale()
{
    // Resolve the scale factor. Fractional path takes precedence when both
    // the fractional-scale protocol AND viewporter are available and the
    // compositor has sent a preferred_scale event. Otherwise fall back to
    // the integer scale (defaults to 1 if no preferred_buffer_scale event).
    float newScale = 1.0f;
    if (haveFractionalScale_ && viewport_) {
        newScale = static_cast<float>(fractionalScale120_) / static_cast<float>(kFractionalScaleUnit);
    } else {
        newScale = static_cast<float>(integerScale_);
    }
    if (newScale <= 0.0f) {
        newScale = 1.0f;
    }

    const bool scaleChanged = (newScale != currentScale_);
    currentScale_           = newScale;

    // Logical size may be 0 before the first configure. Skip applying
    // anything until we know the surface size.
    if (logicalW_ <= 0 || logicalH_ <= 0) {
        return;
    }

    // Physical = logical × scale, rounded to the nearest integer. The
    // viewport destination stays at logical size; Dawn renders to the
    // physical-size surface.
    const int newPhysW = static_cast<int>(static_cast<float>(logicalW_) * currentScale_ + 0.5f);
    const int newPhysH = static_cast<int>(static_cast<float>(logicalH_) * currentScale_ + 0.5f);

    if (haveFractionalScale_ && viewport_) {
        // Fractional path: tell the compositor "this buffer is at the
        // physical pixel count above, but should be rendered at the
        // logical destination size below."
        wp_viewport_set_destination(viewport_, logicalW_, logicalH_);
    } else if (surface_) {
        // Integer path: declare the buffer scale so the compositor
        // surface-scales for HiDPI without our viewport intervention.
        wl_surface_set_buffer_scale(surface_, integerScale_);
    }

    const bool sizeChanged = (newPhysW != width_) || (newPhysH != height_);
    width_                 = newPhysW;
    height_                = newPhysH;

    // Don't fire during create()'s synchronous roundtrip — PlatformDawn
    // hasn't read the initial values yet. The firstConfigure_ flag isn't
    // sufficient on its own here because applyScale also fires from
    // scale-change events that arrive after first configure, so we gate
    // on both having a callback AND something useful having changed.
    if (sizeChanged && onFramebufferResize) {
        onFramebufferResize(width_, height_);
    }
    if (scaleChanged && onContentScale) {
        onContentScale(currentScale_, currentScale_);
    }

    if (display_) {
        wl_display_flush(display_);
    }
}

// ---------- toplevel state parsing + xdg_activation_v1 (Stage 6) ----------

void WaylandWindow::parseToplevelStates(struct wl_array *states)
{
    // states is a wl_array of uint32_t enum entries. We only care about
    // three: activated (focus), suspended (compositor-paused; useful for
    // OSC 99 o=invisible), resizing (live-resize debounce).
    bool isActivated = false;
    bool isSuspended = false;
    bool isResizing  = false;
    if (states) {
        // wl_array stores `size` raw bytes + `data` pointer. Each entry is
        // a uint32_t enum value per the xdg-shell protocol.
        const auto *base = static_cast<const uint32_t *>(states->data);
        const size_t n   = states->size / sizeof(uint32_t);
        for (size_t i = 0; i < n; ++i) {
            switch (base[i]) {
                case XDG_TOPLEVEL_STATE_ACTIVATED: isActivated = true; break;
                case XDG_TOPLEVEL_STATE_SUSPENDED: isSuspended = true; break;
                case XDG_TOPLEVEL_STATE_RESIZING: isResizing = true; break;
                default: break;
            }
        }
    }

    if (isActivated != toplevelActivated_) {
        toplevelActivated_ = isActivated;
        if (onFocus) {
            // PlatformDawn dedupes via windowHasFocus_ so double-fire with
            // wl_keyboard.enter/leave is harmless.
            onFocus(toplevelActivated_);
        }
    }
    if (isSuspended != toplevelSuspended_) {
        toplevelSuspended_ = isSuspended;
        if (onVisibility) {
            onVisibility(!toplevelSuspended_);
        }
    }
    if (isResizing != toplevelResizing_) {
        // We use the XCB inLiveResize_ flag (defined in Window.h's protected
        // member) so PlatformDawn's inLiveResize() returns true during
        // compositor-driven resize. When the compositor stops sending
        // RESIZING, fire onLiveResizeEnd so PlatformDawn can flush any
        // debounced framebuffer resize work.
        inLiveResize_          = isResizing;
        const bool resizeEnded = (!isResizing && toplevelResizing_);
        toplevelResizing_      = isResizing;
        if (resizeEnded && onLiveResizeEnd) {
            onLiveResizeEnd();
        }
    }
}

void WaylandWindow::onActivationTokenDone(void *data, xdg_activation_token_v1 *token, const char *tokenStr)
{
    auto *self = static_cast<WaylandWindow *>(data);
    if (token != self->pendingActivationTok_) {
        // Stale token from a superseded raise(); destroy + drop.
        xdg_activation_token_v1_destroy(token);
        return;
    }
    if (self->activation_ && self->surface_ && tokenStr) {
        xdg_activation_v1_activate(self->activation_, tokenStr, self->surface_);
        if (self->display_) {
            wl_display_flush(self->display_);
        }
    }
    xdg_activation_token_v1_destroy(token);
    self->pendingActivationTok_ = nullptr;
}

void WaylandWindow::cancelPendingActivation()
{
    if (pendingActivationTok_) {
        xdg_activation_token_v1_destroy(pendingActivationTok_);
        pendingActivationTok_ = nullptr;
    }
}

void WaylandWindow::raise()
{
    // No xdg_activation_v1 global → no way to ask the compositor to
    // activate us. Compositors typically reserve activation for tokens
    // minted in response to a user gesture (notification click, IPC
    // launch, etc.); a self-generated token is almost always demoted to
    // "demands attention" / urgency hint. Best-effort either way; match
    // the XCB backend's "no-op-if-no-mechanism" stance.
    if (!activation_ || !surface_) {
        return;
    }
    cancelPendingActivation();
    pendingActivationTok_ = xdg_activation_v1_get_activation_token(activation_);
    if (!pendingActivationTok_) {
        return;
    }
    xdg_activation_token_v1_add_listener(pendingActivationTok_, &kActivationTokenListener, this);
    if (lastInputSerial_ && seat_) {
        xdg_activation_token_v1_set_serial(pendingActivationTok_, lastInputSerial_, seat_);
    }
    xdg_activation_token_v1_set_app_id(pendingActivationTok_, kAppId);
    xdg_activation_token_v1_set_surface(pendingActivationTok_, surface_);
    xdg_activation_token_v1_commit(pendingActivationTok_);
    if (display_) {
        wl_display_flush(display_);
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
    // wl_surface listener exposes preferred_buffer_scale (wl_compositor v6+)
    // — the integer-scale fallback used when fractional-scale isn't
    // advertised. Older compositors that don't emit this event leave
    // integerScale_ at its default of 1.
    wl_surface_add_listener(surface_, &kSurfaceListener, this);

    // Stage 5: attach the fractional-scale device + viewport if available.
    // Both are required for the fractional path; if only one is present we
    // fall back to integer scaling via preferred_buffer_scale.
    if (viewporter_) {
        viewport_ = wp_viewporter_get_viewport(viewporter_, surface_);
    }
    if (fractionalMgr_ && viewport_) {
        fractionalScale_ = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalMgr_, surface_);
        if (fractionalScale_) {
            wp_fractional_scale_v1_add_listener(fractionalScale_, &kFractionalScaleListener, this);
        }
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

    if (!cursorShapeMgr_) {
        spdlog::info("WaylandWindow: wp_cursor_shape_v1 not advertised; cursor stays as compositor default");
    }
    if (fractionalMgr_ && viewport_) {
        spdlog::info("WaylandWindow: HiDPI path = wp_fractional_scale_v1 + wp_viewporter");
    } else if (viewporter_ && !fractionalMgr_) {
        spdlog::info("WaylandWindow: HiDPI path = wp_viewporter only (no fractional manager); integer-scale fallback");
    } else if (!viewporter_) {
        spdlog::info("WaylandWindow: HiDPI path = integer-scale only (no wp_viewporter)");
    }
    if (!activation_) {
        spdlog::info("WaylandWindow: xdg_activation_v1 not advertised; raise() is a no-op");
    }

    spdlog::info("WaylandWindow: created {}x{} ({}, scale={})", width_, height_, title, currentScale_);
    return true;
}

void WaylandWindow::destroy()
{
    cancelKeyRepeat();
    cancelAllSelectionReads();
    cancelPendingActivation();
    releaseSeatSelections();
    primarySelMgr_ = nullptr; // registry global; freed by wl_display_disconnect
    dataDeviceMgr_ = nullptr; // ditto

    if (wlFd_ >= 0) {
        loop_.removeFd(wlFd_);
        wlFd_ = -1;
    }
    if (cursorShapeDev_) {
        wp_cursor_shape_device_v1_destroy(cursorShapeDev_);
        cursorShapeDev_ = nullptr;
    }
    if (cursorShapeMgr_) {
        wp_cursor_shape_manager_v1_destroy(cursorShapeMgr_);
        cursorShapeMgr_ = nullptr;
    }
    if (pointer_) {
        wl_pointer_release(pointer_);
        pointer_ = nullptr;
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
    // Per-surface scale/viewport objects must be destroyed before the
    // surface they're attached to.
    if (fractionalScale_) {
        wp_fractional_scale_v1_destroy(fractionalScale_);
        fractionalScale_ = nullptr;
    }
    if (viewport_) {
        wp_viewport_destroy(viewport_);
        viewport_ = nullptr;
    }
    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    for (auto &info : outputs_) {
        if (info.output) {
            wl_output_destroy(info.output);
        }
    }
    outputs_.clear();
    seat_          = nullptr;
    wmBase_        = nullptr;
    compositor_    = nullptr;
    registry_      = nullptr;
    viewporter_    = nullptr; // registry global; freed by wl_display_disconnect
    fractionalMgr_ = nullptr; // ditto
    activation_    = nullptr; // ditto
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
    x = y = currentScale_;
}

void WaylandWindow::getScreenSize(int &w, int &h) const
{
    // Prefer an output the surface is currently on; if multiple, return
    // the largest (treated as "the screen we'd most want to size the
    // texture pool for"). If none have entered yet (e.g. called before
    // the first surface.enter), fall back to the largest known output.
    // Returns 0×0 only if no wl_output has been bound at all, in which
    // case PlatformDawn's clamp falls through to the texture-pool floor.
    int bestW       = 0;
    int bestH       = 0;
    bool anyEntered = false;
    for (const auto &info : outputs_) {
        if (info.entered) {
            anyEntered = true;
            if (info.width * info.height > bestW * bestH) {
                bestW = info.width;
                bestH = info.height;
            }
        }
    }
    if (!anyEntered) {
        for (const auto &info : outputs_) {
            if (info.width * info.height > bestW * bestH) {
                bestW = info.width;
                bestH = info.height;
            }
        }
    }
    w = bestW;
    h = bestH;
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

void WaylandWindow::setCursorStyle(CursorStyle shape)
{
    if (shape == currentCursor_) {
        return;
    }
    currentCursor_ = shape;
    applyCursorShape();
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
