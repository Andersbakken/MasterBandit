#pragma once

#include <EventLoop.h>
#include <Window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_surface;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct wl_data_device_manager;
struct wl_data_device;
struct wl_data_source;
struct wl_data_offer;
struct zwp_primary_selection_device_manager_v1;
struct zwp_primary_selection_device_v1;
struct zwp_primary_selection_source_v1;
struct zwp_primary_selection_offer_v1;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

// Stage 1-5 Wayland backend. Opens a wl_surface + xdg_toplevel, dispatches
// Wayland events from the EpollEventLoop fd, hands the surface to Dawn via
// SurfaceSourceWaylandSurface (Stage 1), processes keyboard input
// (xkbcommon-backed keymap, modifiers, client-synthesized key repeat) via
// wl_seat + wl_keyboard (Stage 2), handles pointer events + cursor shape
// via wl_pointer + wp_cursor_shape_v1 (Stage 3), bridges clipboard +
// primary-selection round-trips via wl_data_device + zwp_primary_selection_v1
// (Stage 4), and adapts to compositor-reported scale (integer via
// wl_surface.preferred_buffer_scale fallback, fractional via
// wp_fractional_scale_v1 + wp_viewporter primary path) for HiDPI displays
// (Stage 5).
//
// Stage 6 (polish, multi-compositor verification, xdg_activation_v1) lands
// in the final patch.
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

    void setClipboard(const std::string &text) override;
    void setPrimarySelection(const std::string &text) override;
    void requestSelection(SelectionSource src, SelectionCallback cb) override;

    std::string keyName(int keycode) const override;
    uint32_t shiftedKeyCodepoint(int keycode) const override;
    uint32_t baseLayoutKeyCodepoint(int keycode) const override;

    void setCursorStyle(CursorStyle shape) override;

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
    static const struct wl_pointer_listener kPointerListener;
    static const struct wl_data_device_listener kDataDeviceListener;
    static const struct wl_data_source_listener kDataSourceListener;
    static const struct wl_data_offer_listener kDataOfferListener;
    static const struct zwp_primary_selection_device_v1_listener kPrimaryDeviceListener;
    static const struct zwp_primary_selection_source_v1_listener kPrimarySourceListener;
    static const struct zwp_primary_selection_offer_v1_listener kPrimaryOfferListener;
    static const struct wl_surface_listener kSurfaceListener;
    static const struct wp_fractional_scale_v1_listener kFractionalScaleListener;

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

    static void onPointerEnter(void *data, wl_pointer *pointer, uint32_t serial, wl_surface *surface, int32_t /*wl_fixed_t*/ surfaceX, int32_t /*wl_fixed_t*/ surfaceY);
    static void onPointerLeave(void *data, wl_pointer *pointer, uint32_t serial, wl_surface *surface);
    static void onPointerMotion(void *data, wl_pointer *pointer, uint32_t time, int32_t /*wl_fixed_t*/ surfaceX, int32_t /*wl_fixed_t*/ surfaceY);
    static void onPointerButton(void *data, wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
    static void onPointerAxis(void *data, wl_pointer *pointer, uint32_t time, uint32_t axis, int32_t /*wl_fixed_t*/ value);
    static void onPointerFrame(void *data, wl_pointer *pointer);
    static void onPointerAxisSource(void *data, wl_pointer *pointer, uint32_t axis_source);
    static void onPointerAxisStop(void *data, wl_pointer *pointer, uint32_t time, uint32_t axis);
    static void onPointerAxisDiscrete(void *data, wl_pointer *pointer, uint32_t axis, int32_t discrete);

    // Clipboard (wl_data_device)
    static void onDataDeviceDataOffer(void *data, wl_data_device *device, wl_data_offer *offer);
    static void onDataDeviceEnter(void *data, wl_data_device *device, uint32_t serial, wl_surface *surface, int32_t x, int32_t y, wl_data_offer *offer);
    static void onDataDeviceLeave(void *data, wl_data_device *device);
    static void onDataDeviceMotion(void *data, wl_data_device *device, uint32_t time, int32_t x, int32_t y);
    static void onDataDeviceDrop(void *data, wl_data_device *device);
    static void onDataDeviceSelection(void *data, wl_data_device *device, wl_data_offer *offer);

    static void onDataOfferOffer(void *data, wl_data_offer *offer, const char *mime);
    static void onDataOfferSourceActions(void *data, wl_data_offer *offer, uint32_t source_actions);
    static void onDataOfferAction(void *data, wl_data_offer *offer, uint32_t dnd_action);

    static void onDataSourceTarget(void *data, wl_data_source *source, const char *mime);
    static void onDataSourceSend(void *data, wl_data_source *source, const char *mime, int32_t fd);
    static void onDataSourceCancelled(void *data, wl_data_source *source);
    static void onDataSourceDndDropPerformed(void *data, wl_data_source *source);
    static void onDataSourceDndFinished(void *data, wl_data_source *source);
    static void onDataSourceAction(void *data, wl_data_source *source, uint32_t dnd_action);

    // Primary selection (zwp_primary_selection_v1)
    static void onPrimaryDeviceDataOffer(void *data, zwp_primary_selection_device_v1 *device, zwp_primary_selection_offer_v1 *offer);
    static void onPrimaryDeviceSelection(void *data, zwp_primary_selection_device_v1 *device, zwp_primary_selection_offer_v1 *offer);

    static void onPrimaryOfferOffer(void *data, zwp_primary_selection_offer_v1 *offer, const char *mime);

    static void onPrimarySourceSend(void *data, zwp_primary_selection_source_v1 *source, const char *mime, int32_t fd);
    static void onPrimarySourceCancelled(void *data, zwp_primary_selection_source_v1 *source);

    // HiDPI / fractional scale (Stage 5)
    static void onSurfaceEnter(void *data, wl_surface *surface, struct wl_output *output);
    static void onSurfaceLeave(void *data, wl_surface *surface, struct wl_output *output);
    static void onSurfacePreferredBufferScale(void *data, wl_surface *surface, int32_t factor);
    static void onSurfacePreferredBufferTransform(void *data, wl_surface *surface, uint32_t transform);
    static void onFractionalScalePreferredScale(void *data, wp_fractional_scale_v1 *device, uint32_t scale120ths);

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

    // Cursor helpers. Cursor-shape-v1 is the only path; if the compositor
    // doesn't advertise the protocol, applyCursorShape becomes a no-op and
    // the cursor renders as whatever the compositor's default is.
    void attachPointer(wl_pointer *pointer);
    void releasePointer();
    void applyCursorShape();

    // HiDPI helpers. applyScale recomputes physical from logical * scale,
    // updates viewport destination / set_buffer_scale as appropriate, fires
    // onContentScale + onFramebufferResize when the values changed. It's
    // the single funnel for scale or configure changes.
    void applyScale();

    // Selection (clipboard + primary). attachSeatSelections is called once
    // the seat is known to bind the per-seat wl_data_device +
    // zwp_primary_selection_device_v1. setClipboard / setPrimarySelection
    // route through writeSelectionToFd to push our content to a peer's
    // pipe when `send` fires. requestSelection's read path uses
    // startSelectionRead to pipe + watch + accumulate.
    void attachSeatSelections();
    void releaseSeatSelections();
    void resetClipboardSource();
    void resetPrimarySource();
    void writeStringToFd(int fd, const std::string &text);
    void startSelectionRead(int readFd, SelectionCallback cb);
    void cancelAllSelectionReads();

    // Per-offer mime tracking. Kept in a map keyed by the offer pointer so
    // we can drop entries when offers are destroyed without leaking.
    struct OfferMimes
    {
        std::vector<std::string> mimes;
    };

    EventLoop &loop_;

    wl_display *display_       = nullptr;
    wl_registry *registry_     = nullptr;
    wl_compositor *compositor_ = nullptr;
    xdg_wm_base *wmBase_       = nullptr;
    wl_seat *seat_             = nullptr;
    wl_keyboard *keyboard_     = nullptr;
    wl_pointer *pointer_       = nullptr;

    // cursor-shape-v1: per-pointer "tell the compositor which themed cursor
    // to draw" API. Manager comes from the registry (optional global);
    // device is created when a wl_pointer attaches. If the protocol isn't
    // advertised, both stay null and the cursor is whatever the compositor
    // assigns by default.
    wp_cursor_shape_manager_v1 *cursorShapeMgr_ = nullptr;
    wp_cursor_shape_device_v1 *cursorShapeDev_  = nullptr;

    // HiDPI / fractional scale. The fractional-scale + viewporter pair is
    // the modern primary path: the compositor sends a `preferred_scale`
    // event (units = scale × 120) and we set the viewport destination to
    // the logical size while rendering at logical × scale physical pixels.
    // If those protocols aren't advertised, we fall back to integer scale
    // from wl_surface.preferred_buffer_scale (since wl_compositor v6) and
    // call wl_surface.set_buffer_scale(N). Both code paths produce the
    // same physical framebuffer dimensions.
    wp_viewporter *viewporter_                     = nullptr;
    wp_viewport *viewport_                         = nullptr;
    wp_fractional_scale_manager_v1 *fractionalMgr_ = nullptr;
    wp_fractional_scale_v1 *fractionalScale_       = nullptr;
    bool haveFractionalScale_                      = false; // true after first preferred_scale event
    int fractionalScale120_                        = 120;   // last preferred_scale value (120 = 1.0)
    int integerScale_                              = 1;     // last preferred_buffer_scale (or 1 default)
    float currentScale_                            = 1.0f;  // resolved scale applied to physical sizing

    // Clipboard (wl_data_device path).
    wl_data_device_manager *dataDeviceMgr_ = nullptr;
    wl_data_device *dataDevice_            = nullptr;
    wl_data_source *clipboardSource_       = nullptr; // active source we own; null if no copy is live
    wl_data_offer *currentClipboardOffer_  = nullptr; // latest offer from the compositor; null if no selection
    std::string clipboardContent_;                    // data we hand out when `send` arrives on clipboardSource_

    // Primary selection (zwp_primary_selection_v1 path). Mirrors the
    // clipboard wiring. Both managers are optional globals — if the
    // compositor doesn't advertise them, the corresponding setter / reader
    // becomes a no-op + std::nullopt callback.
    zwp_primary_selection_device_manager_v1 *primarySelMgr_ = nullptr;
    zwp_primary_selection_device_v1 *primarySelDevice_      = nullptr;
    zwp_primary_selection_source_v1 *primarySource_         = nullptr;
    zwp_primary_selection_offer_v1 *currentPrimaryOffer_    = nullptr;
    std::string primaryContent_;

    // Per-offer MIME accumulators. Populated as `offer.offer` events arrive
    // between data_offer and selection; consulted by requestSelection() to
    // pick a MIME for the receive request. Cleared when the corresponding
    // offer is destroyed.
    std::unordered_map<wl_data_offer *, OfferMimes> dataOfferMimes_;
    std::unordered_map<zwp_primary_selection_offer_v1 *, OfferMimes> primaryOfferMimes_;

    // Async pipe reads driven by requestSelection. Each entry owns its read-
    // end fd and accumulates bytes until EOF (or timeout). The callback
    // fires exactly once and the entry is dropped.
    struct PendingSelectionRead
    {
        int fd { -1 };
        std::vector<char> buf;
        SelectionCallback cb;
        EventLoop::TimerId timeoutTimer { 0 };
    };

    std::vector<std::unique_ptr<PendingSelectionRead>> pendingReads_;

    // Most-recent serial from any input event (keyboard.enter/.key,
    // pointer.enter/.button). set_selection / set_primary_selection require
    // a serial that the compositor recognises as coming from this client;
    // we keep the freshest one.
    uint32_t lastInputSerial_ = 0;

    // Set true between pointer.enter and pointer.leave on our surface.
    // Cursor shape can only be set with the latest enter serial, so we
    // record it and re-apply on every enter (compositors may reset the
    // shape across enter/leave boundaries).
    bool hasPointerFocus_        = false;
    uint32_t pointerEnterSerial_ = 0;
    CursorStyle currentCursor_   = CursorStyle::IBeam;

    wl_surface *surface_     = nullptr;
    xdg_surface *xdgSurface_ = nullptr;
    xdg_toplevel *toplevel_  = nullptr;

    int wlFd_ = -1;

    // xdg_toplevel.configure delivers LOGICAL dimensions; with Stage 5's
    // scale support, the physical framebuffer is logical × currentScale_.
    // pendingLogicalW/H = latched from the most recent toplevel.configure
    // until xdg_surface.configure acks. logicalW/H = currently-committed
    // logical size. width_/height_ = physical pixels (what Dawn renders to
    // and what getFramebufferSize returns).
    int pendingLogicalW_ = 0;
    int pendingLogicalH_ = 0;
    int logicalW_        = 0;
    int logicalH_        = 0;
    int width_           = 0;
    int height_          = 0;
    int defaultWidth_    = 800;
    int defaultHeight_   = 600;

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
