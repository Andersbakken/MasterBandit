#include "Window_xcb.h"

#include <InputTypes.h>
#include <xkb/Xkb.h>

#include <dawn/webgpu_cpp.h>

// Rename Xlib's Window typedef so it doesn't clash with our class Window
#define Window XlibWindow
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#undef Window

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_util.h>
// xcb/xkb.h uses "explicit" as a C field name which is reserved in C++.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

// Undefine Xlib macros that clash with other code
#ifdef Success
#undef Success
#endif
#ifdef None
#undef None
#endif

void XCBWindow::updateXKBMods()
{
    xkbMods_.shift   = xkb_keymap_mod_get_index(xkbKeymap_, XKB_MOD_NAME_SHIFT);
    xkbMods_.lock    = xkb_keymap_mod_get_index(xkbKeymap_, XKB_MOD_NAME_CAPS);
    xkbMods_.control = xkb_keymap_mod_get_index(xkbKeymap_, XKB_MOD_NAME_CTRL);
    xkbMods_.mod1    = xkb_keymap_mod_get_index(xkbKeymap_, "Mod1");
    xkbMods_.mod2    = xkb_keymap_mod_get_index(xkbKeymap_, "Mod2");
    xkbMods_.mod3    = xkb_keymap_mod_get_index(xkbKeymap_, "Mod3");
    xkbMods_.mod4    = xkb_keymap_mod_get_index(xkbKeymap_, "Mod4");
    xkbMods_.mod5    = xkb_keymap_mod_get_index(xkbKeymap_, "Mod5");
}

void XCBWindow::updateXKBStateFromCore(uint16_t state)
{
    if (!xkbState_) {
        return;
    }

    // Map X11 modifier mask bits to xkb mod index bitmask
    xkb_mod_mask_t xkbMask = 0;
    if ((state & XCB_MOD_MASK_SHIFT) && xkbMods_.shift != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.shift);
    }
    if ((state & XCB_MOD_MASK_LOCK) && xkbMods_.lock != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.lock);
    }
    if ((state & XCB_MOD_MASK_CONTROL) && xkbMods_.control != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.control);
    }
    if ((state & XCB_MOD_MASK_1) && xkbMods_.mod1 != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.mod1);
    }
    if ((state & XCB_MOD_MASK_2) && xkbMods_.mod2 != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.mod2);
    }
    if ((state & XCB_MOD_MASK_3) && xkbMods_.mod3 != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.mod3);
    }
    if ((state & XCB_MOD_MASK_4) && xkbMods_.mod4 != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.mod4);
    }
    if ((state & XCB_MOD_MASK_5) && xkbMods_.mod5 != XKB_MOD_INVALID) {
        xkbMask |= (1u << xkbMods_.mod5);
    }

    // Preserve latched/locked from current state, put the rest in depressed
    xkb_mod_mask_t depressed = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t latched   = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t locked    = xkb_state_serialize_mods(xkbState_, XKB_STATE_MODS_LOCKED);

    latched &= xkbMask;
    locked &= xkbMask;
    depressed &= xkbMask;
    // Any bits in xkbMask not accounted for go into depressed
    depressed |= ~(depressed | latched | locked) & xkbMask;

    // Group from bits 13-14 of X11 state
    xkb_layout_index_t group = (state >> 13) & 3;

    xkb_state_update_mask(xkbState_, depressed, latched, locked, 0, 0, group);
}

// ---------- constructor / destructor ----------

XCBWindow::XCBWindow(EventLoop &loop)
    : loop_(loop)
{
}

XCBWindow::~XCBWindow()
{
    destroy();
}

// ---------- cursors ----------

static xcb_cursor_t createGlyphCursor(xcb_connection_t *conn, xcb_font_t font, uint16_t glyph)
{
    xcb_cursor_t cursor = xcb_generate_id(conn);
    xcb_create_glyph_cursor(conn, cursor, font, font, glyph, glyph + 1, 0, 0, 0, // fg: black
                            0xFFFF,
                            0xFFFF,
                            0xFFFF); // bg: white
    return cursor;
}

void XCBWindow::createCursors()
{
    // Names follow the freedesktop cursor-name spec
    // (https://www.freedesktop.org/wiki/Specifications/cursor-spec/).
    // libxcb-cursor reads the user's gtk-cursor-theme-name / Xcursor.theme
    // and resolves to themed PNGs. Despite what the spec implies, on a
    // name miss xcb_cursor_load_cursor returns 0 with no automatic glyph
    // fallback (and on systems with no theme configured at all,
    // xcb_cursor_context_new can fail outright). Setting XCB_CW_CURSOR to
    // 0 leaves the window inheriting the root window's cursor (the big
    // "X"), so we need our own fallback to the legacy cursor-font glyphs.
    // Glyph indices are from <X11/cursorfont.h>.
    struct CursorSpec
    {
        xcb_cursor_t XCBWindow::*member;
        const char *themeName;
        uint16_t glyph;
    };

    // The cursor font lacks a true not-allowed/forbidden glyph;
    // X_cursor (0) is the closest.
    static const CursorSpec specs[] = {
        { &XCBWindow::cursorArrow_, "default", 68 },           // XC_left_ptr
        { &XCBWindow::cursorIBeam_, "text", 152 },             // XC_xterm
        { &XCBWindow::cursorPointer_, "pointer", 60 },         // XC_hand2
        { &XCBWindow::cursorCrosshair_, "crosshair", 34 },     // XC_crosshair
        { &XCBWindow::cursorWait_, "wait", 150 },              // XC_watch
        { &XCBWindow::cursorHelp_, "help", 92 },               // XC_question_arrow
        { &XCBWindow::cursorMove_, "move", 52 },               // XC_fleur
        { &XCBWindow::cursorNotAllowed_, "not-allowed", 0 },   // XC_X_cursor
        { &XCBWindow::cursorResizeH_, "ew-resize", 108 },      // XC_sb_h_double_arrow
        { &XCBWindow::cursorResizeV_, "ns-resize", 116 },      // XC_sb_v_double_arrow
        { &XCBWindow::cursorResizeNESW_, "nesw-resize", 136 }, // XC_top_right_corner
        { &XCBWindow::cursorResizeNWSE_, "nwse-resize", 134 }, // XC_top_left_corner
    };

    bool haveCtx = (xcb_cursor_context_new(conn_, screen_, &cursorCtx_) >= 0);
    if (!haveCtx) {
        spdlog::warn("XCBWindow: xcb_cursor_context_new failed; "
                     "falling back to legacy cursor-font glyphs");
        cursorCtx_ = nullptr;
    }

    // Open the legacy cursor font lazily (only if we actually need a fallback).
    xcb_font_t glyphFont = 0;
    bool glyphFontOpened = false;
    auto ensureGlyphFont = [&]()
    {
        if (glyphFontOpened) {
            return;
        }
        glyphFontOpened = true;
        glyphFont       = xcb_generate_id(conn_);
        xcb_open_font(conn_, glyphFont, 6, "cursor");
    };

    for (const auto &s : specs) {
        xcb_cursor_t c = 0;
        if (haveCtx) {
            c = xcb_cursor_load_cursor(cursorCtx_, s.themeName);
        }
        if (!c) {
            ensureGlyphFont();
            c = createGlyphCursor(conn_, glyphFont, s.glyph);
        }
        this->*(s.member) = c;
    }

    if (glyphFontOpened) {
        xcb_close_font(conn_, glyphFont);
    }
}

void XCBWindow::setCursorStyle(CursorStyle shape)
{
    if (shape == currentCursor_ || !conn_ || !window_) {
        return;
    }
    currentCursor_ = shape;
    xcb_cursor_t c;
    switch (shape) {
        case CursorStyle::Arrow: c = cursorArrow_; break;
        case CursorStyle::IBeam: c = cursorIBeam_; break;
        case CursorStyle::Pointer: c = cursorPointer_; break;
        case CursorStyle::Crosshair: c = cursorCrosshair_; break;
        case CursorStyle::Wait: c = cursorWait_; break;
        case CursorStyle::Help: c = cursorHelp_; break;
        case CursorStyle::Move: c = cursorMove_; break;
        case CursorStyle::NotAllowed: c = cursorNotAllowed_; break;
        case CursorStyle::ResizeH: c = cursorResizeH_; break;
        case CursorStyle::ResizeV: c = cursorResizeV_; break;
        case CursorStyle::ResizeNESW: c = cursorResizeNESW_; break;
        case CursorStyle::ResizeNWSE: c = cursorResizeNWSE_; break;
        default: return;
    }
    if (!c) {
        return; // never replace a valid cursor with XCB_CURSOR_NONE
    }
    xcb_change_window_attributes(conn_, window_, XCB_CW_CURSOR, &c);
    xcb_flush(conn_);
}

// ---------- create / destroy ----------

bool XCBWindow::create(int width, int height, const std::string &title)
{
    // Open via Xlib so we have a Display* for the Dawn surface (SurfaceSourceXlibWindow)
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        spdlog::error("XCBWindow: XOpenDisplay failed");
        return false;
    }
    // Get the XCB connection from the Xlib display and hand event queue ownership to XCB
    conn_ = XGetXCBConnection(display_);
    if (!conn_ || xcb_connection_has_error(conn_)) {
        spdlog::error("XCBWindow: XGetXCBConnection failed");
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    XSetEventQueueOwner(display_, XCBOwnsEventQueue);

    int screenNum = XDefaultScreen(display_);

    // Get screen
    const xcb_setup_t *setup   = xcb_get_setup(conn_);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNum; ++i) {
        xcb_screen_next(&iter);
    }
    screen_ = iter.data;

    // Intern atoms
    atomWmProtocols_         = internAtom("WM_PROTOCOLS");
    atomWmDeleteWindow_      = internAtom("WM_DELETE_WINDOW");
    atomNetWmSyncRequest_    = internAtom("_NET_WM_SYNC_REQUEST");
    atomNetWmSyncRequestCtr_ = internAtom("_NET_WM_SYNC_REQUEST_COUNTER");
    atomNetWmState_          = internAtom("_NET_WM_STATE");
    atomNetWmStateHidden_    = internAtom("_NET_WM_STATE_HIDDEN");
    atomClipboard_           = internAtom("CLIPBOARD");
    atomPrimary_             = XCB_ATOM_PRIMARY;
    atomTargets_             = internAtom("TARGETS");
    atomUtf8String_          = internAtom("UTF8_STRING");
    atomMbSelection_         = internAtom("MB_SELECTION");

    // Create window
    window_           = xcb_generate_id(conn_);
    // XCB_BACK_PIXMAP_NONE: preserve old content during swapchain transitions
    // instead of flashing black (the default background pixel).
    uint32_t mask     = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        XCB_BACK_PIXMAP_NONE,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_EXPOSURE |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_FOCUS_CHANGE |
            // VisibilityChange feeds the "occluded" input to onVisibility (kitty
            // OSC 99 o=invisible parity); PropertyChange picks up _NET_WM_STATE
            // updates so the iconified bit tracks WM-driven hide/minimize.
            XCB_EVENT_MASK_VISIBILITY_CHANGE |
            XCB_EVENT_MASK_PROPERTY_CHANGE
    };
    xcb_create_window(conn_,
                      XCB_COPY_FROM_PARENT,
                      window_,
                      screen_->root,
                      0,
                      0,
                      static_cast<uint16_t>(width),
                      static_cast<uint16_t>(height),
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen_->root_visual,
                      mask,
                      values);

    // Create mouse cursors and set default to I-beam (terminal window)
    createCursors();
    currentCursor_ = CursorStyle::IBeam;
    xcb_change_window_attributes(conn_, window_, XCB_CW_CURSOR, &cursorIBeam_);

    // Create XSync counter for _NET_WM_SYNC_REQUEST (eliminates resize flicker)
    syncCounter_ = xcb_generate_id(conn_);
    xcb_sync_create_counter(conn_, syncCounter_, { 0, 0 });
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atomNetWmSyncRequestCtr_, XCB_ATOM_CARDINAL, 32, 1, &syncCounter_);

    // Set WM_PROTOCOLS: WM_DELETE_WINDOW + _NET_WM_SYNC_REQUEST
    xcb_atom_t protocols[] = { atomWmDeleteWindow_, atomNetWmSyncRequest_ };
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atomWmProtocols_, XCB_ATOM_ATOM, 32, 2, protocols);

    // WM_CLASS — two NUL-terminated strings: instance, class. GNOME Shell's
    // notification daemon traces a notification's sender PID back to its
    // window and reads WM_CLASS to derive the source label. Match the same
    // reverse-DNS string we put in the desktop-entry hint so a single
    // identifier covers both paths and a future .desktop file resolves
    // cleanly.
    static const char kWmClass[] = "it.masterband.mb\0it.masterband.mb";
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(kWmClass) - 1, kWmClass);

    setTitle(title);
    xcb_map_window(conn_, window_);
    // Request keyboard focus immediately after mapping
    xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, window_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    // Push an initial focus state. The X server hasn't sent us a FocusIn
    // yet — under "focus stealing prevention" (Mutter, KWin with the
    // setting on) the SetInputFocus above may have been denied by the WM,
    // in which case no FocusIn ever arrives and a cached default would
    // misreport our state. GetInputFocus is a round-trip that flushes
    // pending requests; the reply reflects whatever the WM ended up with.
    {
        auto cookie = xcb_get_input_focus(conn_);
        if (auto *r = xcb_get_input_focus_reply(conn_, cookie, nullptr)) {
            bool focused = (r->focus == window_);
            free(r);
            if (onFocus) {
                onFocus(focused);
            }
        }
    }

    // Query actual geometry — a tiling WM may have already forced a
    // different size during mapping, without sending a ConfigureNotify.
    auto geomCookie = xcb_get_geometry(conn_, window_);
    if (auto *geom = xcb_get_geometry_reply(conn_, geomCookie, nullptr)) {
        width_  = geom->width;
        height_ = geom->height;
        free(geom);
    } else {
        width_  = width;
        height_ = height;
    }

    // xkbcommon setup
    xkbCtx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkbCtx_) {
        spdlog::error("XCBWindow: xkb_context_new failed");
        return false;
    }

    // Use xkbcommon-x11 to get the keymap from the server
    uint8_t xkbFirstEvent = 0;
    if (xkb_x11_setup_xkb_extension(conn_,
                                    XKB_X11_MIN_MAJOR_XKB_VERSION,
                                    XKB_X11_MIN_MINOR_XKB_VERSION,
                                    XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                    nullptr,
                                    nullptr,
                                    &xkbFirstEvent,
                                    nullptr) != 1) {
        spdlog::warn("XCBWindow: XKB extension not available, falling back to default keymap");
        xkbKeymap_ = xkb_keymap_new_from_names(xkbCtx_, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    } else {
        xkbEventBase_ = xkbFirstEvent;
        xkbDeviceId_  = xkb_x11_get_core_keyboard_device_id(conn_);
        xkbKeymap_    = xkb_x11_keymap_new_from_device(xkbCtx_, conn_, xkbDeviceId_, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }

    if (!xkbKeymap_) {
        spdlog::error("XCBWindow: failed to create xkb keymap");
        return false;
    }

    updateXKBMods();

    xkbState_ = xkb_x11_state_new_from_device(xkbKeymap_, conn_, xkbDeviceId_);
    if (!xkbState_) {
        spdlog::error("XCBWindow: xkb_x11_state_new_from_device failed");
        return false;
    }

    // Clean state on the same keymap — no modifiers applied. Used for
    // kitty CSI-u unshifted keyCode. We only sync the layout group on
    // updates; depressed/latched/locked are kept at zero.
    xkbCleanState_ = xkb_state_new(xkbKeymap_);
    if (!xkbCleanState_) {
        spdlog::warn("XCBWindow: xkb_state_new(clean) failed; unshifted keyCode disabled");
    }

    // Default-rules keymap for kitty `base_layout_key`. Built once from
    // empty xkb_rule_names so XKB resolves to the system default
    // (XKB_DEFAULT_LAYOUT, typically "us"). Failure is non-fatal — we just
    // skip emitting the third CSI u field. Mirrors kitty xkb_glfw.c:594.
    {
        xkb_rule_names defaultRules = { };
        xkbDefaultKeymap_           = xkb_keymap_new_from_names(xkbCtx_, &defaultRules, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (xkbDefaultKeymap_) {
            xkbDefaultState_ = xkb_state_new(xkbDefaultKeymap_);
            if (!xkbDefaultState_) {
                spdlog::warn("XCBWindow: xkb_state_new(default) failed; base_layout_key disabled");
                xkb_keymap_unref(xkbDefaultKeymap_);
                xkbDefaultKeymap_ = nullptr;
            }
        } else {
            spdlog::warn("XCBWindow: default keymap unavailable; base_layout_key disabled");
        }
    }

    // Subscribe to XKB state change events for modifier tracking.
    if (xkbEventBase_) {
        xcb_xkb_select_events(conn_,
                              XCB_XKB_ID_USE_CORE_KBD,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                              0,
                              XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                              0,
                              0,
                              nullptr);
    }

    // Register the XCB fd with the event loop
    int fd = xcb_get_file_descriptor(conn_);
    loop_.watchFd(fd, EventLoop::FdEvents::Readable, [this](EventLoop::FdEvents)
                  {
                      processEvents();
                  });

    // Tiling WMs may resize the window after mapping without sending a
    // ConfigureNotify that we can observe.  Schedule a one-shot geometry
    // check once the event loop starts so we pick up the real size.
    loop_.addTimer(0, false, [this]()
                   {
                       auto cookie = xcb_get_geometry(conn_, window_);
                       if (auto *geom = xcb_get_geometry_reply(conn_, cookie, nullptr)) {
                           if (geom->width != width_ || geom->height != height_) {
                               width_  = geom->width;
                               height_ = geom->height;
                               if (onFramebufferResize) {
                                   onFramebufferResize(width_, height_);
                               }
                           }
                           free(geom);
                       }
                   });

    return true;
}

void XCBWindow::destroy()
{
    if (selectionSweepTimer_ != 0) {
        loop_.removeTimer(selectionSweepTimer_);
        selectionSweepTimer_ = 0;
    }
    pendingSelections_.clear(); // drop callbacks; nothing to deliver to during shutdown

    if (conn_) {
        int fd = xcb_get_file_descriptor(conn_);
        loop_.removeFd(fd);
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

    if (conn_) {
        xcb_cursor_t *all[] = {
            &cursorArrow_,
            &cursorIBeam_,
            &cursorPointer_,
            &cursorCrosshair_,
            &cursorWait_,
            &cursorHelp_,
            &cursorMove_,
            &cursorNotAllowed_,
            &cursorResizeH_,
            &cursorResizeV_,
            &cursorResizeNESW_,
            &cursorResizeNWSE_,
        };
        for (xcb_cursor_t *c : all) {
            if (*c) {
                xcb_free_cursor(conn_, *c);
                *c = 0;
            }
        }
        if (cursorCtx_) {
            xcb_cursor_context_free(cursorCtx_);
            cursorCtx_ = nullptr;
        }
        if (window_) {
            xcb_destroy_window(conn_, window_);
        }
        window_ = 0;
        conn_   = nullptr; // owned by Xlib display, don't disconnect separately
    }
    if (display_) {
        XCloseDisplay(display_); // closes the underlying XCB connection too
        display_ = nullptr;
    }
}

// ---------- properties ----------

void XCBWindow::raise()
{
    if (!conn_ || !window_) {
        return;
    }
    // EWMH _NET_ACTIVE_WINDOW client message. source_indication = 2 means
    // "request from a pager / taskbar / notification daemon proxy" — most
    // EWMH-compliant compositors honor source=2 without a focus-stealing
    // demotion. source=1 (normal app) is more frequently demoted to an
    // urgency hint. We don't have an activation token (X11 has no such
    // concept) so this is best-effort by design; some compositors
    // (notably some tiling WMs) ignore _NET_ACTIVE_WINDOW entirely.
    xcb_atom_t atomNetActiveWindow = internAtom("_NET_ACTIVE_WINDOW");
    if (atomNetActiveWindow != 0) {
        xcb_client_message_event_t ev { };
        ev.response_type  = XCB_CLIENT_MESSAGE;
        ev.format         = 32;
        ev.window         = window_;
        ev.type           = atomNetActiveWindow;
        ev.data.data32[0] = 2;                // source: pager
        ev.data.data32[1] = XCB_CURRENT_TIME; // timestamp
        ev.data.data32[2] = 0;                // requestor's currently-active window (none)
        ev.data.data32[3] = 0;
        ev.data.data32[4] = 0;
        xcb_send_event(conn_, 0, screen_->root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, reinterpret_cast<const char *>(&ev));
    }
    // Also raise + request input focus directly. Compositors that honor
    // _NET_ACTIVE_WINDOW will overwrite these; on simpler WMs this is the
    // fallback that actually works.
    const uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn_, window_, XCB_CONFIG_WINDOW_STACK_MODE, values);
    xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, window_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

void XCBWindow::setTitle(const std::string &title)
{
    if (!conn_ || !window_) {
        return;
    }
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, static_cast<uint32_t>(title.size()), title.c_str());
    // Also set _NET_WM_NAME for UTF-8 compositors
    xcb_atom_t netWmName = internAtom("_NET_WM_NAME");
    xcb_atom_t utf8      = atomUtf8String_;
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, netWmName, utf8, 8, static_cast<uint32_t>(title.size()), title.c_str());
    xcb_flush(conn_);
}

void XCBWindow::getFramebufferSize(int &w, int &h) const
{
    w = width_;
    h = height_;
}

void XCBWindow::getContentScale(float &x, float &y) const
{
    // X11 screen DPI from server, fallback to 1.0
    // For HiDPI detection we'd query Xrandr; keep simple for now
    x = y = 1.0f;
}

void XCBWindow::getScreenSize(int &w, int &h) const
{
    if (screen_) {
        w = screen_->width_in_pixels;
        h = screen_->height_in_pixels;
    } else {
        w = h = 0;
    }
}

int XCBWindow::connectionFd() const
{
    return conn_ ? xcb_get_file_descriptor(conn_) : -1;
}

// ---------- WebGPU surface ----------

wgpu::Surface XCBWindow::createWgpuSurface(wgpu::Instance instance)
{
    // Dawn on Linux only supports SurfaceSourceXlibWindow, not XCB
    wgpu::SurfaceSourceXlibWindow xlibSource;
    xlibSource.display = display_;
    xlibSource.window  = static_cast<uint64_t>(window_);

    wgpu::SurfaceDescriptor desc;
    desc.nextInChain = &xlibSource;
    return instance.CreateSurface(&desc);
}

// ---------- event processing ----------

void XCBWindow::processEvents()
{
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(conn_)) != nullptr) {
        uint8_t type = event->response_type & ~0x80;
        switch (type) {
            case XCB_KEY_PRESS: {
                auto *ev          = reinterpret_cast<xcb_key_press_event_t *>(event);
                // Detect auto-repeat: same keycode arriving without a true release
                bool isRepeat     = (ev->detail == lastPressKeycode_ && ev->time == lastPressTime_);
                lastPressKeycode_ = ev->detail;
                lastPressTime_    = ev->time;
                handleKeyPress(ev, isRepeat);
                break;
            }
            case XCB_KEY_RELEASE: {
                auto *ev                  = reinterpret_cast<xcb_key_release_event_t *>(event);
                // Peek: if the next event is a KeyPress with the same keycode+time, it's auto-repeat
                xcb_generic_event_t *next = xcb_poll_for_event(conn_);
                if (next) {
                    uint8_t nextType = next->response_type & ~0x80;
                    if (nextType == XCB_KEY_PRESS) {
                        auto *nextEv = reinterpret_cast<xcb_key_press_event_t *>(next);
                        if (nextEv->detail == ev->detail && nextEv->time == ev->time) {
                            // Auto-repeat pair: skip the release, treat the press as repeat.
                            lastPressKeycode_ = nextEv->detail;
                            lastPressTime_    = nextEv->time;
                            handleKeyPress(nextEv, true);
                            free(next);
                            break;
                        }
                    }
                    // Not a repeat: process the release then the next event on next iteration
                    // Re-queue by processing the saved next event immediately
                    handleKeyRelease(ev);
                    // Process next now (recurse into switch by reusing the event)
                    uint8_t nextType2 = next->response_type & ~0x80;
                    switch (nextType2) {
                        case XCB_KEY_PRESS: {
                            auto *n           = reinterpret_cast<xcb_key_press_event_t *>(next);
                            lastPressKeycode_ = n->detail;
                            lastPressTime_    = n->time;
                            handleKeyPress(n, false);
                            break;
                        }
                        default:
                            // Put it back indirectly by processing it here
                            // (small duplication to avoid recursion)
                            break;
                    }
                    free(next);
                } else {
                    handleKeyRelease(ev);
                }
                break;
            }
            case XCB_BUTTON_PRESS: {
                handleButtonPress(reinterpret_cast<xcb_button_press_event_t *>(event));
                break;
            }
            case XCB_BUTTON_RELEASE: {
                handleButtonRelease(reinterpret_cast<xcb_button_release_event_t *>(event));
                break;
            }
            case XCB_MOTION_NOTIFY: {
                handleMotion(reinterpret_cast<xcb_motion_notify_event_t *>(event));
                break;
            }
            case XCB_FOCUS_IN: {
                handleFocusIn(reinterpret_cast<xcb_focus_in_event_t *>(event));
                break;
            }
            case XCB_FOCUS_OUT: {
                handleFocusOut(reinterpret_cast<xcb_focus_out_event_t *>(event));
                break;
            }
            case XCB_CONFIGURE_NOTIFY: {
                handleConfigureNotify(reinterpret_cast<xcb_configure_notify_event_t *>(event));
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                handleClientMessage(reinterpret_cast<xcb_client_message_event_t *>(event));
                break;
            }
            case XCB_SELECTION_REQUEST: {
                handleSelectionRequest(reinterpret_cast<xcb_selection_request_event_t *>(event));
                break;
            }
            case XCB_SELECTION_NOTIFY: {
                handleSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t *>(event));
                break;
            }
            case XCB_EXPOSE:
                if (onExpose) {
                    onExpose();
                }
                break;
            case XCB_VISIBILITY_NOTIFY: {
                handleVisibilityNotify(reinterpret_cast<xcb_visibility_notify_event_t *>(event));
                break;
            }
            case XCB_MAP_NOTIFY: {
                handleMapNotify(reinterpret_cast<xcb_map_notify_event_t *>(event));
                break;
            }
            case XCB_UNMAP_NOTIFY: {
                handleUnmapNotify(reinterpret_cast<xcb_unmap_notify_event_t *>(event));
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                handlePropertyNotify(reinterpret_cast<xcb_property_notify_event_t *>(event));
                break;
            }
            default:
                if (xkbEventBase_ && type == xkbEventBase_) {
                    auto *xkbEvent = reinterpret_cast<xcb_xkb_state_notify_event_t *>(event);
                    if (xkbEvent->xkbType == XCB_XKB_STATE_NOTIFY && xkbState_) {
                        xkb_state_update_mask(xkbState_,
                                              xkbEvent->baseMods,
                                              xkbEvent->latchedMods,
                                              xkbEvent->lockedMods,
                                              xkbEvent->baseGroup,
                                              xkbEvent->latchedGroup,
                                              xkbEvent->lockedGroup);
                        // Keep the clean state's group in sync — modifiers
                        // stay at zero so xkb_state_key_get_utf32 reports
                        // the layout-unshifted codepoint.
                        if (xkbCleanState_) {
                            xkb_state_update_mask(xkbCleanState_, 0, 0, 0, xkbEvent->baseGroup, xkbEvent->latchedGroup, xkbEvent->lockedGroup);
                        }
                    }
                }
                break;
        }
        free(event);
    }
    xcb_flush(conn_);
}

// ---------- input handlers ----------

void XCBWindow::handleKeyPress(xcb_key_press_event_t *ev, bool isRepeat)
{
    if (!xkbState_) {
        return;
    }

    xkb_keycode_t keycode = ev->detail;

    // Sync modifier state from the server's mask embedded in the event.
    // X11 reports `state` as the modifier mask *before* the event, so a
    // bare Ctrl press arrives with state=0 (Ctrl isn't applied yet). To
    // get a post-event mod mask — needed by InputController so Ctrl-hover
    // can flip the cursor to the link-pointer shape without waiting for
    // a subsequent key event — we apply the key to our local xkb state
    // ourselves after syncing. Binding identity uses the level-0 base
    // keysym (independent of mods); the modifier-aware codepoint for PTY
    // emit comes from `xkb_state_key_get_utf32` further down.
    updateXKBStateFromCore(ev->state);

    Key k = mb::xkb::keysymToKey(mb::xkb::baseKeysymForKeycode(xkbState_, xkbKeymap_, keycode));

    xkb_state_update_key(xkbState_, keycode, XKB_KEY_DOWN);

    uint32_t mods    = mb::xkb::stateToModifiers(xkbState_);
    KeyAction action = isRepeat ? KeyAction_Repeat : KeyAction_Press;

    if (onKey) {
        onKey(static_cast<int>(k), static_cast<int>(keycode), static_cast<int>(action), static_cast<int>(mods));
    }

    // Emit char for printable keys (non-control, non-repeat or repeat)
    if (onChar && !(mods & CtrlModifier) && !(mods & MetaModifier)) {
        uint32_t cp = xkb_state_key_get_utf32(xkbState_, keycode);
        if (cp >= 0x20 && cp != 0x7f) {
            // Unshifted codepoint from the clean state (no modifiers
            // applied; layout group tracked). This is the kitty CSI-u
            // keyCode per spec — Shift+A reports keyCode='a', shifted='A'.
            // 0 = unavailable (clean state failed to construct).
            uint32_t unshifted = xkbCleanState_ ? xkb_state_key_get_utf32(xkbCleanState_, keycode) : 0;
            if (unshifted < 0x20 || unshifted == 0x7f) {
                unshifted = 0;
            }
            onChar(cp, unshifted);
        }
    }
}

void XCBWindow::handleKeyRelease(xcb_key_release_event_t *ev)
{
    if (!xkbState_) {
        return;
    }

    xkb_keycode_t keycode = ev->detail;

    // Same pre/post-event distinction as handleKeyPress: ev->state still
    // includes the modifier being released; applying the release to our
    // local xkb state gives a post-event mod mask so InputController
    // sees Ctrl drop on key-up rather than at the next event.
    updateXKBStateFromCore(ev->state);

    Key k = mb::xkb::keysymToKey(mb::xkb::baseKeysymForKeycode(xkbState_, xkbKeymap_, keycode));

    xkb_state_update_key(xkbState_, keycode, XKB_KEY_UP);

    uint32_t mods = mb::xkb::stateToModifiers(xkbState_);

    if (onKey) {
        onKey(static_cast<int>(k), static_cast<int>(keycode), static_cast<int>(KeyAction_Release), static_cast<int>(mods));
    }
}

void XCBWindow::handleButtonPress(xcb_button_press_event_t *ev)
{
    // XCB buttons: 1=left, 2=middle, 3=right, 4/5=scroll
    int button = -1;
    switch (ev->detail) {
        case 1: button = static_cast<int>(LeftButton); break;
        case 2: button = static_cast<int>(MidButton); break;
        case 3: button = static_cast<int>(RightButton); break;
        case 4:
            if (onScroll) {
                onScroll(0.0, 1.0);
            }
            return;
        case 5:
            if (onScroll) {
                onScroll(0.0, -1.0);
            }
            return;
        case 6:
            if (onScroll) {
                onScroll(-1.0, 0.0);
            }
            return;
        case 7:
            if (onScroll) {
                onScroll(1.0, 0.0);
            }
            return;
        default: return;
    }
    uint32_t mods = xkbState_ ? mb::xkb::stateToModifiers(xkbState_) : 0;
    if (onMouseButton) {
        onMouseButton(button, static_cast<int>(KeyAction_Press), static_cast<int>(mods));
    }
    if (onCursorPos) {
        onCursorPos(ev->event_x, ev->event_y);
    }
}

void XCBWindow::handleButtonRelease(xcb_button_release_event_t *ev)
{
    int button = -1;
    switch (ev->detail) {
        case 1: button = static_cast<int>(LeftButton); break;
        case 2: button = static_cast<int>(MidButton); break;
        case 3: button = static_cast<int>(RightButton); break;
        default: return;
    }
    uint32_t mods = xkbState_ ? mb::xkb::stateToModifiers(xkbState_) : 0;
    if (onMouseButton) {
        onMouseButton(button, static_cast<int>(KeyAction_Release), static_cast<int>(mods));
    }
}

void XCBWindow::handleMotion(xcb_motion_notify_event_t *ev)
{
    if (onCursorPos) {
        onCursorPos(ev->event_x, ev->event_y);
    }
}

void XCBWindow::handleFocusIn(xcb_focus_in_event_t *)
{
    if (onFocus) {
        onFocus(true);
    }
}

void XCBWindow::handleFocusOut(xcb_focus_out_event_t *)
{
    if (onFocus) {
        onFocus(false);
    }
}

void XCBWindow::handleVisibilityNotify(xcb_visibility_notify_event_t *ev)
{
    // XCB_VISIBILITY_FULLY_OBSCURED == 2: every pixel of the window is
    // covered by other windows. Treat partially-obscured/unobscured as
    // visible (matches kitty/glfw GLFW_OCCLUDED, which only flips on
    // VisibilityFullyObscured per the X11 protocol).
    bool obscured = (ev->state == XCB_VISIBILITY_FULLY_OBSCURED);
    if (obscured == fullyObscured_) {
        return;
    }
    fullyObscured_ = obscured;
    refreshVisibility();
}

void XCBWindow::handleMapNotify(xcb_map_notify_event_t *)
{
    if (mapped_) {
        return;
    }
    mapped_ = true;
    refreshVisibility();
}

void XCBWindow::handleUnmapNotify(xcb_unmap_notify_event_t *)
{
    if (!mapped_) {
        return;
    }
    mapped_ = false;
    refreshVisibility();
}

void XCBWindow::handlePropertyNotify(xcb_property_notify_event_t *ev)
{
    if (atomNetWmState_ && ev->atom == atomNetWmState_) {
        updateIconifiedFromNetWmState();
    }
}

void XCBWindow::updateIconifiedFromNetWmState()
{
    // _NET_WM_STATE is a list of atoms. Iconified ⇔ _NET_WM_STATE_HIDDEN
    // is in the list (set by mutter/kwin/openbox/etc. on minimize). Modern
    // WMs use _NET_WM_STATE_HIDDEN in preference to the legacy WM_STATE
    // IconicState; we check the new path only.
    if (!atomNetWmState_ || !atomNetWmStateHidden_) {
        return;
    }
    xcb_get_property_cookie_t c = xcb_get_property(
        conn_,
        0,
        window_,
        atomNetWmState_,
        XCB_ATOM_ATOM,
        0,
        64);
    xcb_generic_error_t *err    = nullptr;
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn_, c, &err);
    if (err) {
        free(err);
    }
    bool hidden = false;
    if (r) {
        if (r->type == XCB_ATOM_ATOM && r->format == 32) {
            int n                   = xcb_get_property_value_length(r) / 4;
            const xcb_atom_t *atoms = static_cast<const xcb_atom_t *>(
                xcb_get_property_value(r));
            for (int i = 0; i < n; ++i) {
                if (atoms[i] == atomNetWmStateHidden_) {
                    hidden = true;
                    break;
                }
            }
        }
        free(r);
    }
    if (hidden == iconified_) {
        return;
    }
    iconified_ = hidden;
    refreshVisibility();
}

void XCBWindow::refreshVisibility()
{
    bool nowVisible = mapped_ && !iconified_ && !fullyObscured_;
    if (nowVisible == visible_) {
        return;
    }
    visible_ = nowVisible;
    if (onVisibility) {
        onVisibility(nowVisible);
    }
}

void XCBWindow::handleConfigureNotify(xcb_configure_notify_event_t *ev)
{
    if (ev->width != width_ || ev->height != height_) {
        width_  = ev->width;
        height_ = ev->height;
        // Cancel any existing debounce timer and start a fresh 100ms one-shot.
        // While the timer is pending, inLiveResize() returns true so SIGWINCH
        // is deferred. When the timer fires it clears the flag and wakes the
        // event loop so renderFrame() runs and flushPendingResize() is called.
        if (resizeDebounceTimer_) {
            loop_.removeTimer(resizeDebounceTimer_);
            resizeDebounceTimer_ = 0;
        }
        inLiveResize_        = true;
        resizeDebounceTimer_ = loop_.addTimer(100, false, [this]()
                                              {
                                                  resizeDebounceTimer_ = 0;
                                                  inLiveResize_        = false;
                                                  if (onLiveResizeEnd) {
                                                      onLiveResizeEnd();
                                                  }
                                              });
        if (onFramebufferResize) {
            onFramebufferResize(width_, height_);
        }
    }
}

bool XCBWindow::inLiveResize() const
{
    return inLiveResize_;
}

void XCBWindow::handleClientMessage(xcb_client_message_event_t *ev)
{
    if (ev->type != atomWmProtocols_) {
        return;
    }
    if (ev->data.data32[0] == atomWmDeleteWindow_) {
        shouldClose_ = true;
    } else if (ev->data.data32[0] == atomNetWmSyncRequest_) {
        // Acknowledge immediately so the compositor never stalls waiting for us.
        int64_t serial = static_cast<int64_t>(
            static_cast<uint64_t>(ev->data.data32[3]) << 32 |
            static_cast<uint64_t>(ev->data.data32[2]));
        if (serial && syncCounter_) {
            xcb_sync_int64_t value;
            value.hi = static_cast<int32_t>(serial >> 32);
            value.lo = static_cast<uint32_t>(serial);
            xcb_sync_set_counter(conn_, syncCounter_, value);
            xcb_flush(conn_);
        }
    }
}

// ---------- clipboard ----------

xcb_atom_t XCBWindow::internAtom(const char *name, bool onlyIfExists) const
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn_, onlyIfExists ? 1 : 0, static_cast<uint16_t>(strlen(name)), name);
    xcb_intern_atom_reply_t *reply  = xcb_intern_atom_reply(conn_, cookie, nullptr);
    if (!reply) {
        return XCB_ATOM_NONE;
    }
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

void XCBWindow::setClipboard(const std::string &text)
{
    clipboardContent_ = text;
    xcb_set_selection_owner(conn_, window_, atomClipboard_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

// All clipboard reads on XCB go through requestSelection() (defined below),
// which registers a pending entry and resolves on SELECTION_NOTIFY without
// blocking the main thread. There used to be sync getClipboard /
// getPrimarySelection here; they were busy-polls that raced with the async
// path and have been removed.

void XCBWindow::setPrimarySelection(const std::string &text)
{
    primaryContent_ = text;
    xcb_set_selection_owner(conn_, window_, atomPrimary_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

std::string XCBWindow::readSelectionProperty(xcb_atom_t property) const
{
    xcb_get_property_cookie_t c = xcb_get_property(conn_, 1 /* delete */, window_, property, XCB_ATOM_ANY, 0, UINT32_MAX);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn_, c, nullptr);
    if (!r) {
        return { };
    }
    std::string result(static_cast<const char *>(xcb_get_property_value(r)),
                       xcb_get_property_value_length(r));
    free(r);
    return result;
}

void XCBWindow::handleSelectionRequest(xcb_selection_request_event_t *ev)
{
    xcb_selection_notify_event_t notify { };
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.requestor     = ev->requestor;
    notify.selection     = ev->selection;
    notify.target        = ev->target;
    notify.property      = XCB_ATOM_NONE;
    notify.time          = ev->time;

    const std::string &content = (ev->selection == atomPrimary_)
        ? primaryContent_
        : clipboardContent_;

    if (ev->target == atomTargets_) {
        xcb_atom_t supported[] = { atomUtf8String_, XCB_ATOM_STRING };
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, ev->requestor, ev->property, XCB_ATOM_ATOM, 32, 2, supported);
        notify.property = ev->property;
    } else if (ev->target == atomUtf8String_ || ev->target == XCB_ATOM_STRING) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, ev->requestor, ev->property, ev->target, 8, static_cast<uint32_t>(content.size()), content.c_str());
        notify.property = ev->property;
    }

    xcb_send_event(conn_, 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&notify));
    xcb_flush(conn_);
}

// Async selection: see Window.h::requestSelection. Callers fire-and-forget;
// completion lands here when the SELECTION_NOTIFY event arrives, or via
// sweepStaleSelectionRequests() when the deadline passes. This is the only
// clipboard read path now — sync getClipboard / getPrimarySelection were
// removed because their busy-polls could steal SELECTION_NOTIFY events
// meant for a pending async request.
static uint64_t monotonicMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void XCBWindow::requestSelection(SelectionSource src, SelectionCallback cb)
{
    if (!conn_) {
        cb(std::nullopt);
        return;
    }
    xcb_atom_t selection = (src == SelectionSource::Primary) ? atomPrimary_ : atomClipboard_;

    // If we own the selection, satisfy from in-memory content immediately.
    xcb_get_selection_owner_cookie_t c = xcb_get_selection_owner(conn_, selection);
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(conn_, c, nullptr);
    if (r && r->owner == window_) {
        const std::string &s = (selection == atomPrimary_) ? primaryContent_ : clipboardContent_;
        free(r);
        if (s.empty()) {
            cb(std::nullopt);
        } else {
            cb(s);
        }
        return;
    }
    if (r) {
        free(r);
    }

    xcb_convert_selection(conn_, window_, selection, atomUtf8String_, atomMbSelection_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    constexpr uint64_t kTimeoutMs = 5000;
    pendingSelections_.push_back({ selection, std::move(cb), monotonicMs() + kTimeoutMs });

    if (selectionSweepTimer_ == 0) {
        selectionSweepTimer_ = loop_.addTimer(250, true, [this]()
                                              {
                                                  sweepStaleSelectionRequests();
                                              });
    }
}

void XCBWindow::sweepStaleSelectionRequests()
{
    if (pendingSelections_.empty()) {
        return;
    }
    const uint64_t now = monotonicMs();
    std::vector<SelectionCallback> toFire;
    pendingSelections_.erase(
        std::remove_if(pendingSelections_.begin(), pendingSelections_.end(), [&](PendingSelectionRequest &p)
                       {
                           if (p.deadlineMs <= now) {
                               toFire.push_back(std::move(p.cb));
                               return true;
                           }
                           return false;
                       }),
        pendingSelections_.end());
    if (pendingSelections_.empty() && selectionSweepTimer_ != 0) {
        loop_.removeTimer(selectionSweepTimer_);
        selectionSweepTimer_ = 0;
    }
    for (auto &cb : toFire) {
        cb(std::nullopt);
    }
}

void XCBWindow::handleSelectionNotify(xcb_selection_notify_event_t *ev)
{
    auto it = std::find_if(pendingSelections_.begin(), pendingSelections_.end(), [&](const PendingSelectionRequest &p)
                           {
                               return p.selection == ev->selection;
                           });
    if (it == pendingSelections_.end()) {
        return; // unsolicited or already swept
    }
    SelectionCallback cb = std::move(it->cb);
    pendingSelections_.erase(it);
    if (pendingSelections_.empty() && selectionSweepTimer_ != 0) {
        loop_.removeTimer(selectionSweepTimer_);
        selectionSweepTimer_ = 0;
    }

    if (ev->property == XCB_ATOM_NONE) {
        cb(std::nullopt);
        return;
    }
    std::string text = readSelectionProperty(atomMbSelection_);
    if (text.empty()) {
        cb(std::nullopt);
    } else {
        cb(std::move(text));
    }
}

// ---------- key name ----------

std::string XCBWindow::keyName(int keycode) const
{
    return mb::xkb::keyName(xkbState_, keycode);
}

uint32_t XCBWindow::shiftedKeyCodepoint(int keycode) const
{
    return mb::xkb::shiftedKeyCodepoint(xkbState_, xkbKeymap_, keycode);
}

uint32_t XCBWindow::baseLayoutKeyCodepoint(int keycode) const
{
    return mb::xkb::baseLayoutKeyCodepoint(xkbDefaultKeymap_, keycode);
}
