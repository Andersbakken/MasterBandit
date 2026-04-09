#include "Window_xcb.h"

#include <InputTypes.h>

#include <dawn/webgpu_cpp.h>

// Rename Xlib's Window typedef so it doesn't clash with our class Window
#define Window XlibWindow
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#undef Window

#include <xcb/xcb.h>
#include <xcb/xcb_util.h>
// xcb/xkb.h uses "explicit" as a C field name which is reserved in C++.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>

// Undefine Xlib macros that clash with other code
#ifdef Success
#  undef Success
#endif
#ifdef None
#  undef None
#endif

// ---------- keysym → Key translation table (adapted from GLFW x11_init.c) ----------

static Key keysymToKey(xkb_keysym_t sym)
{
    // Latin-1 and ASCII printable range maps directly
    if (sym >= 0x20 && sym <= 0x7e)
        return static_cast<Key>(sym);
    if (sym >= 0xa0 && sym <= 0xff)
        return static_cast<Key>(sym);

    switch (sym) {
    case XKB_KEY_Escape:       return Key_Escape;
    case XKB_KEY_Tab:          return Key_Tab;
    case XKB_KEY_ISO_Left_Tab: return Key_Backtab;
    case XKB_KEY_BackSpace:    return Key_Backspace;
    case XKB_KEY_Return:       return Key_Return;
    case XKB_KEY_KP_Enter:     return Key_Enter;
    case XKB_KEY_Insert:       return Key_Insert;
    case XKB_KEY_Delete:       return Key_Delete;
    case XKB_KEY_Pause:        return Key_Pause;
    case XKB_KEY_Print:        return Key_Print;
    case XKB_KEY_Home:         return Key_Home;
    case XKB_KEY_End:          return Key_End;
    case XKB_KEY_Left:         return Key_Left;
    case XKB_KEY_Up:           return Key_Up;
    case XKB_KEY_Right:        return Key_Right;
    case XKB_KEY_Down:         return Key_Down;
    case XKB_KEY_Page_Up:      return Key_PageUp;
    case XKB_KEY_Page_Down:    return Key_PageDown;
    case XKB_KEY_Shift_L:      return Key_Shift_L;
    case XKB_KEY_Shift_R:      return Key_Shift_R;
    case XKB_KEY_Control_L:    return Key_Control_L;
    case XKB_KEY_Control_R:    return Key_Control_R;
    case XKB_KEY_Alt_L:        return Key_Alt_L;
    case XKB_KEY_Alt_R:        return Key_Alt_R;
    case XKB_KEY_Super_L:      return Key_Super_L;
    case XKB_KEY_Super_R:      return Key_Super_R;
    case XKB_KEY_Hyper_L:      return Key_Hyper_L;
    case XKB_KEY_Hyper_R:      return Key_Hyper_R;
    case XKB_KEY_Menu:         return Key_Menu;
    case XKB_KEY_Help:         return Key_Help;
    case XKB_KEY_Caps_Lock:    return Key_CapsLock;
    case XKB_KEY_Num_Lock:     return Key_NumLock;
    case XKB_KEY_Scroll_Lock:  return Key_ScrollLock;
    case XKB_KEY_F1:           return Key_F1;
    case XKB_KEY_F2:           return Key_F2;
    case XKB_KEY_F3:           return Key_F3;
    case XKB_KEY_F4:           return Key_F4;
    case XKB_KEY_F5:           return Key_F5;
    case XKB_KEY_F6:           return Key_F6;
    case XKB_KEY_F7:           return Key_F7;
    case XKB_KEY_F8:           return Key_F8;
    case XKB_KEY_F9:           return Key_F9;
    case XKB_KEY_F10:          return Key_F10;
    case XKB_KEY_F11:          return Key_F11;
    case XKB_KEY_F12:          return Key_F12;
    case XKB_KEY_F13:          return Key_F13;
    case XKB_KEY_F14:          return Key_F14;
    case XKB_KEY_F15:          return Key_F15;
    case XKB_KEY_F16:          return Key_F16;
    case XKB_KEY_F17:          return Key_F17;
    case XKB_KEY_F18:          return Key_F18;
    case XKB_KEY_F19:          return Key_F19;
    case XKB_KEY_F20:          return Key_F20;
    case XKB_KEY_F21:          return Key_F21;
    case XKB_KEY_F22:          return Key_F22;
    case XKB_KEY_F23:          return Key_F23;
    case XKB_KEY_F24:          return Key_F24;
    case XKB_KEY_F25:          return Key_F25;
    case XKB_KEY_F26:          return Key_F26;
    case XKB_KEY_F27:          return Key_F27;
    case XKB_KEY_F28:          return Key_F28;
    case XKB_KEY_F29:          return Key_F29;
    case XKB_KEY_F30:          return Key_F30;
    case XKB_KEY_F31:          return Key_F31;
    case XKB_KEY_F32:          return Key_F32;
    case XKB_KEY_F33:          return Key_F33;
    case XKB_KEY_F34:          return Key_F34;
    case XKB_KEY_F35:          return Key_F35;
    case XKB_KEY_KP_0:         return Key_KP_0;
    case XKB_KEY_KP_1:         return Key_KP_1;
    case XKB_KEY_KP_2:         return Key_KP_2;
    case XKB_KEY_KP_3:         return Key_KP_3;
    case XKB_KEY_KP_4:         return Key_KP_4;
    case XKB_KEY_KP_5:         return Key_KP_5;
    case XKB_KEY_KP_6:         return Key_KP_6;
    case XKB_KEY_KP_7:         return Key_KP_7;
    case XKB_KEY_KP_8:         return Key_KP_8;
    case XKB_KEY_KP_9:         return Key_KP_9;
    case XKB_KEY_KP_Decimal:   return Key_KP_Decimal;
    case XKB_KEY_KP_Divide:    return Key_KP_Divide;
    case XKB_KEY_KP_Multiply:  return Key_KP_Multiply;
    case XKB_KEY_KP_Subtract:  return Key_KP_Subtract;
    case XKB_KEY_KP_Add:       return Key_KP_Add;
    case XKB_KEY_KP_Equal:     return Key_KP_Equal;
    case XKB_KEY_Mode_switch:  return Key_Mode_switch;
    case XKB_KEY_Multi_key:    return Key_Multi_key;
    default:                   return Key_unknown;
    }
}

static uint32_t xkbStateToModifiers(xkb_state* state)
{
    uint32_t m = 0;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= ShiftModifier;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= CtrlModifier;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= AltModifier;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= MetaModifier;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= CapsLockModifier;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) m |= NumLockModifier;
    return m;
}

// ---------- constructor / destructor ----------

XCBWindow::XCBWindow(EventLoop& loop) : loop_(loop) {}

XCBWindow::~XCBWindow()
{
    destroy();
}

// ---------- cursors ----------

static xcb_cursor_t createGlyphCursor(xcb_connection_t* conn, xcb_font_t font, uint16_t glyph)
{
    xcb_cursor_t cursor = xcb_generate_id(conn);
    xcb_create_glyph_cursor(conn, cursor, font, font,
        glyph, glyph + 1,
        0xFFFF, 0xFFFF, 0xFFFF,  // fg: white
        0, 0, 0);                // bg: black
    return cursor;
}

void XCBWindow::createCursors()
{
    xcb_font_t font = xcb_generate_id(conn_);
    xcb_open_font(conn_, font, 6, "cursor");
    cursorArrow_   = createGlyphCursor(conn_, font, 68);  // XC_left_ptr
    cursorIBeam_   = createGlyphCursor(conn_, font, 152); // XC_xterm
    cursorResizeH_ = createGlyphCursor(conn_, font, 108); // XC_sb_h_double_arrow
    cursorResizeV_ = createGlyphCursor(conn_, font, 116); // XC_sb_v_double_arrow
    xcb_close_font(conn_, font);
}

void XCBWindow::setCursorStyle(CursorStyle shape)
{
    if (shape == currentCursor_ || !conn_ || !window_) return;
    currentCursor_ = shape;
    xcb_cursor_t c;
    switch (shape) {
    case CursorStyle::Arrow:   c = cursorArrow_; break;
    case CursorStyle::IBeam:   c = cursorIBeam_; break;
    case CursorStyle::ResizeH: c = cursorResizeH_; break;
    case CursorStyle::ResizeV: c = cursorResizeV_; break;
    default: return;
    }
    xcb_change_window_attributes(conn_, window_, XCB_CW_CURSOR, &c);
    xcb_flush(conn_);
}

// ---------- create / destroy ----------

bool XCBWindow::create(int width, int height, const std::string& title)
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
        XCloseDisplay(display_); display_ = nullptr;
        return false;
    }
    XSetEventQueueOwner(display_, XCBOwnsEventQueue);

    int screenNum = XDefaultScreen(display_);

    // Get screen
    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNum; ++i)
        xcb_screen_next(&iter);
    screen_ = iter.data;

    // Intern atoms
    atomWmProtocols_         = internAtom("WM_PROTOCOLS");
    atomWmDeleteWindow_      = internAtom("WM_DELETE_WINDOW");
    atomNetWmSyncRequest_    = internAtom("_NET_WM_SYNC_REQUEST");
    atomNetWmSyncRequestCtr_ = internAtom("_NET_WM_SYNC_REQUEST_COUNTER");
    atomClipboard_           = internAtom("CLIPBOARD");
    atomPrimary_             = XCB_ATOM_PRIMARY;
    atomTargets_             = internAtom("TARGETS");
    atomUtf8String_          = internAtom("UTF8_STRING");
    atomMbSelection_         = internAtom("MB_SELECTION");

    // Create window
    window_ = xcb_generate_id(conn_);
    // XCB_BACK_PIXMAP_NONE: preserve old content during swapchain transitions
    // instead of flashing black (the default background pixel).
    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        XCB_BACK_PIXMAP_NONE,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_FOCUS_CHANGE
    };
    xcb_create_window(conn_,
        XCB_COPY_FROM_PARENT, window_, screen_->root,
        0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
        mask, values);

    // Create mouse cursors and set default to I-beam (terminal window)
    createCursors();
    currentCursor_ = CursorStyle::IBeam;
    xcb_change_window_attributes(conn_, window_, XCB_CW_CURSOR, &cursorIBeam_);

    // Create XSync counter for _NET_WM_SYNC_REQUEST (eliminates resize flicker)
    syncCounter_ = xcb_generate_id(conn_);
    xcb_sync_create_counter(conn_, syncCounter_, {0, 0});
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
        atomNetWmSyncRequestCtr_, XCB_ATOM_CARDINAL, 32, 1, &syncCounter_);

    // Set WM_PROTOCOLS: WM_DELETE_WINDOW + _NET_WM_SYNC_REQUEST
    xcb_atom_t protocols[] = { atomWmDeleteWindow_, atomNetWmSyncRequest_ };
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
        atomWmProtocols_, XCB_ATOM_ATOM, 32, 2, protocols);

    setTitle(title);
    xcb_map_window(conn_, window_);
    // Request keyboard focus immediately after mapping
    xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, window_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    // Query actual geometry — a tiling WM may have already forced a
    // different size during mapping, without sending a ConfigureNotify.
    auto geomCookie = xcb_get_geometry(conn_, window_);
    if (auto* geom = xcb_get_geometry_reply(conn_, geomCookie, nullptr)) {
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
            XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            nullptr, nullptr, &xkbFirstEvent, nullptr) != 1) {
        spdlog::warn("XCBWindow: XKB extension not available, falling back to default keymap");
        xkbKeymap_ = xkb_keymap_new_from_names(xkbCtx_, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    } else {
        xkbEventBase_ = xkbFirstEvent;
        xkbDeviceId_ = xkb_x11_get_core_keyboard_device_id(conn_);
        xkbKeymap_   = xkb_x11_keymap_new_from_device(xkbCtx_, conn_, xkbDeviceId_,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);

        // Subscribe to XKB state change events so modifier state stays in
        // sync even when key releases are consumed by the WM (e.g. during
        // keyboard grabs for desktop switching).
        xcb_xkb_select_events(conn_,
            XCB_XKB_ID_USE_CORE_KBD,
            XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
            0,
            XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
            0, 0, nullptr);
    }

    if (!xkbKeymap_) {
        spdlog::error("XCBWindow: failed to create xkb keymap");
        return false;
    }

    xkbState_ = xkb_state_new(xkbKeymap_);
    if (!xkbState_) {
        spdlog::error("XCBWindow: xkb_state_new failed");
        return false;
    }

    // Register the XCB fd with the event loop
    int fd = xcb_get_file_descriptor(conn_);
    loop_.watchFd(fd, EventLoop::FdEvents::Readable,
        [this](EventLoop::FdEvents) { processEvents(); });

    // Tiling WMs may resize the window after mapping without sending a
    // ConfigureNotify that we can observe.  Schedule a one-shot geometry
    // check once the event loop starts so we pick up the real size.
    loop_.addTimer(0, false, [this]() {
        auto cookie = xcb_get_geometry(conn_, window_);
        if (auto* geom = xcb_get_geometry_reply(conn_, cookie, nullptr)) {
            if (geom->width != width_ || geom->height != height_) {
                width_  = geom->width;
                height_ = geom->height;
                if (onFramebufferResize) onFramebufferResize(width_, height_);
            }
            free(geom);
        }
    });

    return true;
}

void XCBWindow::destroy()
{
    if (conn_) {
        int fd = xcb_get_file_descriptor(conn_);
        loop_.removeFd(fd);
    }

    if (xkbState_)   { xkb_state_unref(xkbState_);   xkbState_   = nullptr; }
    if (xkbKeymap_)  { xkb_keymap_unref(xkbKeymap_);  xkbKeymap_  = nullptr; }
    if (xkbCtx_)     { xkb_context_unref(xkbCtx_);    xkbCtx_     = nullptr; }

    if (conn_) {
        if (cursorArrow_)   xcb_free_cursor(conn_, cursorArrow_);
        if (cursorIBeam_)   xcb_free_cursor(conn_, cursorIBeam_);
        if (cursorResizeH_) xcb_free_cursor(conn_, cursorResizeH_);
        if (cursorResizeV_) xcb_free_cursor(conn_, cursorResizeV_);
        cursorArrow_ = cursorIBeam_ = cursorResizeH_ = cursorResizeV_ = 0;
        if (window_) xcb_destroy_window(conn_, window_);
        window_ = 0;
        conn_   = nullptr;  // owned by Xlib display, don't disconnect separately
    }
    if (display_) {
        XCloseDisplay(display_);  // closes the underlying XCB connection too
        display_ = nullptr;
    }
}

// ---------- properties ----------

void XCBWindow::setTitle(const std::string& title)
{
    if (!conn_ || !window_) return;
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        static_cast<uint32_t>(title.size()), title.c_str());
    // Also set _NET_WM_NAME for UTF-8 compositors
    xcb_atom_t netWmName = internAtom("_NET_WM_NAME");
    xcb_atom_t utf8 = atomUtf8String_;
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
        netWmName, utf8, 8,
        static_cast<uint32_t>(title.size()), title.c_str());
    xcb_flush(conn_);
}

void XCBWindow::getFramebufferSize(int& w, int& h) const
{
    w = width_;
    h = height_;
}

void XCBWindow::getContentScale(float& x, float& y) const
{
    // X11 screen DPI from server, fallback to 1.0
    // For HiDPI detection we'd query Xrandr; keep simple for now
    x = y = 1.0f;
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
    xcb_generic_event_t* event;
    while ((event = xcb_poll_for_event(conn_)) != nullptr) {
        uint8_t type = event->response_type & ~0x80;
        switch (type) {
        case XCB_KEY_PRESS: {
            auto* ev = reinterpret_cast<xcb_key_press_event_t*>(event);
            // Detect auto-repeat: same keycode arriving without a true release
            bool isRepeat = (ev->detail == lastPressKeycode_ && ev->time == lastPressTime_);
            lastPressKeycode_ = ev->detail;
            lastPressTime_    = ev->time;
            handleKeyPress(ev, isRepeat);
            break;
        }
        case XCB_KEY_RELEASE: {
            auto* ev = reinterpret_cast<xcb_key_release_event_t*>(event);
            // Peek: if the next event is a KeyPress with the same keycode+time, it's auto-repeat
            xcb_generic_event_t* next = xcb_poll_for_event(conn_);
            if (next) {
                uint8_t nextType = next->response_type & ~0x80;
                if (nextType == XCB_KEY_PRESS) {
                    auto* nextEv = reinterpret_cast<xcb_key_press_event_t*>(next);
                    if (nextEv->detail == ev->detail && nextEv->time == ev->time) {
                        // Auto-repeat pair: skip the release, treat the press as repeat.
                        // Still feed the release to xkb so modifier state stays balanced.
                        xkb_state_update_key(xkbState_, ev->detail, XKB_KEY_UP);
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
                    auto* n = reinterpret_cast<xcb_key_press_event_t*>(next);
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
            handleButtonPress(reinterpret_cast<xcb_button_press_event_t*>(event));
            break;
        }
        case XCB_BUTTON_RELEASE: {
            handleButtonRelease(reinterpret_cast<xcb_button_release_event_t*>(event));
            break;
        }
        case XCB_MOTION_NOTIFY: {
            handleMotion(reinterpret_cast<xcb_motion_notify_event_t*>(event));
            break;
        }
        case XCB_FOCUS_IN: {
            handleFocusIn(reinterpret_cast<xcb_focus_in_event_t*>(event));
            break;
        }
        case XCB_FOCUS_OUT: {
            handleFocusOut(reinterpret_cast<xcb_focus_out_event_t*>(event));
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            handleConfigureNotify(reinterpret_cast<xcb_configure_notify_event_t*>(event));
            break;
        }
        case XCB_CLIENT_MESSAGE: {
            handleClientMessage(reinterpret_cast<xcb_client_message_event_t*>(event));
            break;
        }
        case XCB_SELECTION_REQUEST: {
            handleSelectionRequest(reinterpret_cast<xcb_selection_request_event_t*>(event));
            break;
        }
        case XCB_SELECTION_NOTIFY: {
            handleSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t*>(event));
            break;
        }
        case XCB_EXPOSE:
            if (onExpose) onExpose();
            break;
        default:
            // XKB extension events arrive with type == xkbEventBase_
            if (xkbEventBase_ && type == xkbEventBase_) {
                auto* xkbEvent = reinterpret_cast<xcb_xkb_state_notify_event_t*>(event);
                if (xkbEvent->xkbType == XCB_XKB_STATE_NOTIFY && xkbState_) {
                    xkb_state_update_mask(xkbState_,
                        xkbEvent->baseMods, xkbEvent->latchedMods, xkbEvent->lockedMods,
                        xkbEvent->baseGroup, xkbEvent->latchedGroup, xkbEvent->lockedGroup);
                }
            }
            break;
        }
        free(event);
    }
    xcb_flush(conn_);
}

// ---------- input handlers ----------

void XCBWindow::handleKeyPress(xcb_key_press_event_t* ev, bool isRepeat)
{
    if (!xkbState_) return;

    xkb_keycode_t keycode = ev->detail;

    // Update xkb state
    xkb_state_update_key(xkbState_, keycode, XKB_KEY_DOWN);

    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_, keycode);
    Key k = keysymToKey(sym);
    uint32_t mods = xkbStateToModifiers(xkbState_);
    KeyAction action = isRepeat ? KeyAction_Repeat : KeyAction_Press;

    if (onKey) onKey(static_cast<int>(k), static_cast<int>(keycode), static_cast<int>(action), static_cast<int>(mods));

    // Emit char for printable keys (non-control, non-repeat or repeat)
    if (onChar && !(mods & CtrlModifier) && !(mods & MetaModifier)) {
        uint32_t cp = xkb_state_key_get_utf32(xkbState_, keycode);
        if (cp >= 0x20 && cp != 0x7f) {
            onChar(cp);
        }
    }
}

void XCBWindow::handleKeyRelease(xcb_key_release_event_t* ev)
{
    if (!xkbState_) return;

    xkb_keycode_t keycode = ev->detail;
    xkb_state_update_key(xkbState_, keycode, XKB_KEY_UP);

    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_, keycode);
    Key k = keysymToKey(sym);
    uint32_t mods = xkbStateToModifiers(xkbState_);

    if (onKey) onKey(static_cast<int>(k), static_cast<int>(keycode),
                      static_cast<int>(KeyAction_Release), static_cast<int>(mods));
}

void XCBWindow::handleButtonPress(xcb_button_press_event_t* ev)
{
    // XCB buttons: 1=left, 2=middle, 3=right, 4/5=scroll
    int button = -1;
    switch (ev->detail) {
    case 1: button = static_cast<int>(LeftButton); break;
    case 2: button = static_cast<int>(MidButton); break;
    case 3: button = static_cast<int>(RightButton); break;
    case 4:
        if (onScroll) onScroll(0.0, 1.0);
        return;
    case 5:
        if (onScroll) onScroll(0.0, -1.0);
        return;
    case 6:
        if (onScroll) onScroll(-1.0, 0.0);
        return;
    case 7:
        if (onScroll) onScroll(1.0, 0.0);
        return;
    default: return;
    }
    uint32_t mods = xkbState_ ? xkbStateToModifiers(xkbState_) : 0;
    if (onMouseButton) onMouseButton(button, static_cast<int>(KeyAction_Press), static_cast<int>(mods));
    if (onCursorPos)   onCursorPos(ev->event_x, ev->event_y);
}

void XCBWindow::handleButtonRelease(xcb_button_release_event_t* ev)
{
    int button = -1;
    switch (ev->detail) {
    case 1: button = static_cast<int>(LeftButton); break;
    case 2: button = static_cast<int>(MidButton); break;
    case 3: button = static_cast<int>(RightButton); break;
    default: return;
    }
    uint32_t mods = xkbState_ ? xkbStateToModifiers(xkbState_) : 0;
    if (onMouseButton) onMouseButton(button, static_cast<int>(KeyAction_Release), static_cast<int>(mods));
}

void XCBWindow::handleMotion(xcb_motion_notify_event_t* ev)
{
    if (onCursorPos) onCursorPos(ev->event_x, ev->event_y);
}

void XCBWindow::handleFocusIn(xcb_focus_in_event_t*)
{
    if (onFocus) onFocus(true);
}

void XCBWindow::handleFocusOut(xcb_focus_out_event_t*)
{
    if (onFocus) onFocus(false);
}

void XCBWindow::handleConfigureNotify(xcb_configure_notify_event_t* ev)
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
        inLiveResize_ = true;
        resizeDebounceTimer_ = loop_.addTimer(100, false, [this]() {
            resizeDebounceTimer_ = 0;
            inLiveResize_ = false;
            if (onLiveResizeEnd) onLiveResizeEnd();
        });
        if (onFramebufferResize) onFramebufferResize(width_, height_);
    }
}

bool XCBWindow::inLiveResize() const
{
    return inLiveResize_;
}

void XCBWindow::handleClientMessage(xcb_client_message_event_t* ev)
{
    if (ev->type != atomWmProtocols_) return;
    if (ev->data.data32[0] == atomWmDeleteWindow_) {
        shouldClose_ = true;
    } else if (ev->data.data32[0] == atomNetWmSyncRequest_) {
        syncSerial_ = static_cast<int64_t>(
            static_cast<uint64_t>(ev->data.data32[3]) << 32 |
            static_cast<uint64_t>(ev->data.data32[2]));
    }
}

void XCBWindow::frameRendered()
{
    if (!syncSerial_ || !syncCounter_) return;
    xcb_sync_int64_t value;
    value.hi = static_cast<int32_t>(syncSerial_ >> 32);
    value.lo = static_cast<uint32_t>(syncSerial_);
    xcb_sync_set_counter(conn_, syncCounter_, value);
    xcb_flush(conn_);
    syncSerial_ = 0;
}

// ---------- clipboard ----------

xcb_atom_t XCBWindow::internAtom(const char* name, bool onlyIfExists) const
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn_, onlyIfExists ? 1 : 0,
                                                       static_cast<uint16_t>(strlen(name)), name);
    xcb_intern_atom_reply_t* reply  = xcb_intern_atom_reply(conn_, cookie, nullptr);
    if (!reply) return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

void XCBWindow::setClipboard(const std::string& text)
{
    clipboardContent_ = text;
    xcb_set_selection_owner(conn_, window_, atomClipboard_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

std::string XCBWindow::getClipboard() const
{
    // Check if we own it
    xcb_get_selection_owner_cookie_t c = xcb_get_selection_owner(conn_, atomClipboard_);
    xcb_get_selection_owner_reply_t* r = xcb_get_selection_owner_reply(conn_, c, nullptr);
    if (r && r->owner == window_) { free(r); return clipboardContent_; }
    if (r) free(r);

    // Request conversion from current owner
    xcb_convert_selection(conn_, window_, atomClipboard_,
                           atomUtf8String_, atomMbSelection_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    // Poll for SelectionNotify (with timeout)
    // Note: this is called from the main thread event processing path,
    // so we do a short busy-wait for the response.
    for (int i = 0; i < 200; ++i) {
        xcb_generic_event_t* event = xcb_poll_for_event(conn_);
        if (!event) {
            struct timespec ts { 0, 5'000'000 }; // 5ms
            nanosleep(&ts, nullptr);
            continue;
        }
        if ((event->response_type & ~0x80) == XCB_SELECTION_NOTIFY) {
            auto* sn = reinterpret_cast<xcb_selection_notify_event_t*>(event);
            if (sn->property != XCB_ATOM_NONE) {
                std::string result = readSelectionProperty(atomMbSelection_);
                free(event);
                return result;
            }
            free(event);
            return {};
        }
        free(event);
    }
    return {};
}

void XCBWindow::setPrimarySelection(const std::string& text)
{
    primaryContent_ = text;
    xcb_set_selection_owner(conn_, window_, atomPrimary_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

std::string XCBWindow::getPrimarySelection() const
{
    xcb_get_selection_owner_cookie_t c = xcb_get_selection_owner(conn_, atomPrimary_);
    xcb_get_selection_owner_reply_t* r = xcb_get_selection_owner_reply(conn_, c, nullptr);
    if (r && r->owner == window_) { free(r); return primaryContent_; }
    if (r) free(r);

    xcb_convert_selection(conn_, window_, atomPrimary_,
                           atomUtf8String_, atomMbSelection_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    for (int i = 0; i < 200; ++i) {
        xcb_generic_event_t* event = xcb_poll_for_event(conn_);
        if (!event) {
            struct timespec ts { 0, 5'000'000 };
            nanosleep(&ts, nullptr);
            continue;
        }
        if ((event->response_type & ~0x80) == XCB_SELECTION_NOTIFY) {
            auto* sn = reinterpret_cast<xcb_selection_notify_event_t*>(event);
            if (sn->property != XCB_ATOM_NONE) {
                std::string result = readSelectionProperty(atomMbSelection_);
                free(event);
                return result;
            }
            free(event);
            return {};
        }
        free(event);
    }
    return {};
}

std::string XCBWindow::readSelectionProperty(xcb_atom_t property) const
{
    xcb_get_property_cookie_t c = xcb_get_property(conn_, 1 /* delete */, window_,
                                                     property, XCB_ATOM_ANY, 0, UINT32_MAX);
    xcb_get_property_reply_t* r = xcb_get_property_reply(conn_, c, nullptr);
    if (!r) return {};
    std::string result(static_cast<const char*>(xcb_get_property_value(r)),
                        xcb_get_property_value_length(r));
    free(r);
    return result;
}

void XCBWindow::handleSelectionRequest(xcb_selection_request_event_t* ev)
{
    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.requestor     = ev->requestor;
    notify.selection     = ev->selection;
    notify.target        = ev->target;
    notify.property      = XCB_ATOM_NONE;
    notify.time          = ev->time;

    const std::string& content = (ev->selection == atomPrimary_)
                                ? primaryContent_ : clipboardContent_;

    if (ev->target == atomTargets_) {
        xcb_atom_t supported[] = { atomUtf8String_, XCB_ATOM_STRING };
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, ev->requestor,
                             ev->property, XCB_ATOM_ATOM, 32, 2, supported);
        notify.property = ev->property;
    } else if (ev->target == atomUtf8String_ || ev->target == XCB_ATOM_STRING) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, ev->requestor,
                             ev->property, ev->target, 8,
                             static_cast<uint32_t>(content.size()), content.c_str());
        notify.property = ev->property;
    }

    xcb_send_event(conn_, 0, ev->requestor, XCB_EVENT_MASK_NO_EVENT,
                    reinterpret_cast<const char*>(&notify));
    xcb_flush(conn_);
}

void XCBWindow::handleSelectionNotify(xcb_selection_notify_event_t* ev)
{
    // Handled inline in getClipboard/getPrimarySelection blocking loop
    (void)ev;
}

// ---------- key name ----------

std::string XCBWindow::keyName(int keycode) const
{
    if (!xkbState_) return {};
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState_,
                                                    static_cast<xkb_keycode_t>(keycode));
    char buf[64];
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    return buf;
}
