#pragma once

#include <Window.h>
#include <EventLoop.h>

#include <xcb/xcb.h>
#include <xcb/sync.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
// Forward-declare Display to avoid pulling in all of Xlib
struct _XDisplay;
typedef struct _XDisplay Display;

#include <cstdint>
#include <string>
#include <vector>

class XCBWindow : public Window {
public:
    explicit XCBWindow(EventLoop& loop);
    ~XCBWindow() override;

    bool create(int width, int height, const std::string& title) override;
    void destroy() override;
    bool shouldClose() const override { return shouldClose_; }

    void setTitle(const std::string& title) override;
    void getFramebufferSize(int& w, int& h) const override;
    void getContentScale(float& x, float& y) const override;

    void        setClipboard(const std::string& text) override;
    std::string getClipboard() const override;
    void        setPrimarySelection(const std::string& text) override;
    std::string getPrimarySelection() const override;

    std::string keyName(int keycode) const override;

    wgpu::Surface createWgpuSurface(wgpu::Instance instance) override;
    void setCursorStyle(CursorStyle shape) override;

    // Called by EpollEventLoop when the XCB fd is readable
    void processEvents();

    xcb_connection_t* connection() const { return conn_; }
    Display*          xlibDisplay() const { return display_; }
    int               connectionFd() const;

private:
    void handleKeyPress(xcb_key_press_event_t* ev, bool isRepeat);
    void handleKeyRelease(xcb_key_release_event_t* ev);
    void handleButtonPress(xcb_button_press_event_t* ev);
    void handleButtonRelease(xcb_button_release_event_t* ev);
    void handleMotion(xcb_motion_notify_event_t* ev);
    void handleFocusIn(xcb_focus_in_event_t* ev);
    void handleFocusOut(xcb_focus_out_event_t* ev);
    void handleConfigureNotify(xcb_configure_notify_event_t* ev);
    void handleClientMessage(xcb_client_message_event_t* ev);
    void handleSelectionRequest(xcb_selection_request_event_t* ev);
    void handleSelectionNotify(xcb_selection_notify_event_t* ev);
    void handleExpose(xcb_expose_event_t* ev);

    xcb_atom_t internAtom(const char* name, bool onlyIfExists = false) const;

    // Selection helpers
    std::string readSelectionProperty(xcb_atom_t property) const;
    void        requestSelection(xcb_atom_t selection) const;

    EventLoop& loop_;

    Display*          display_ = nullptr;  // Xlib display (for Dawn surface + atom queries)
    xcb_connection_t* conn_    = nullptr;  // XCB connection (same underlying connection)
    xcb_window_t      window_  = 0;
    xcb_screen_t*     screen_  = nullptr;

    // xkbcommon
    xkb_context* xkbCtx_   = nullptr;
    xkb_keymap*  xkbKeymap_ = nullptr;
    xkb_state*   xkbState_  = nullptr;
    int32_t      xkbDeviceId_ = -1;
    uint8_t      xkbEventBase_ = 0;  // XKB extension first event code

    // Window state
    int  width_  = 0;
    int  height_ = 0;
    bool shouldClose_ = false;

    // _NET_WM_SYNC_REQUEST: signal the WM after each frame to eliminate resize flicker
    xcb_sync_counter_t syncCounter_      = 0;
    int64_t            syncSerial_       = 0; // serial from last _NET_WM_SYNC_REQUEST

    // Atoms
    xcb_atom_t atomWmProtocols_          = 0;
    xcb_atom_t atomWmDeleteWindow_       = 0;
    xcb_atom_t atomNetWmSyncRequest_     = 0;
    xcb_atom_t atomNetWmSyncRequestCtr_  = 0;
    xcb_atom_t atomClipboard_       = 0;
    xcb_atom_t atomPrimary_         = 0;  // XA_PRIMARY
    xcb_atom_t atomTargets_         = 0;
    xcb_atom_t atomUtf8String_      = 0;
    xcb_atom_t atomMbSelection_     = 0;  // scratch property for incoming data

    // Clipboard ownership and content
    std::string clipboardContent_;
    std::string primaryContent_;

    // Pending GetClipboard request (synchronous via xcb_poll_for_special_event)
    mutable std::string pendingClipboard_;
    mutable bool        clipboardPending_ = false;

    // Key repeat detection: track last key press event sequence + keycode
    xcb_keycode_t lastPressKeycode_ = 0;
    uint32_t      lastPressTime_    = 0;

    // Cached X11 cursors (from cursor font)
    xcb_cursor_t cursorArrow_ = 0;
    xcb_cursor_t cursorIBeam_ = 0;
    xcb_cursor_t cursorResizeH_ = 0;
    xcb_cursor_t cursorResizeV_ = 0;
    CursorStyle  currentCursor_ = CursorStyle::Arrow;
    void createCursors();


    // Live resize debounce: set while a one-shot timer is pending after a resize
    EventLoop::TimerId resizeDebounceTimer_ = 0;
    bool inLiveResize() const override;
    void frameRendered() override;
};
