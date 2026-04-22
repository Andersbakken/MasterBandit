#pragma once

#include "Action.h"
#include "Bindings.h"
#include "ClickDetector.h"
#include "InputTypes.h"

#include <eventloop/EventLoop.h>
#include <eventloop/Window.h>

#include "Tab.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Script { class Engine; }
class TerminalEmulator;

// Owns keyboard and mouse input state and dispatch logic.  Constructed by
// PlatformDawn, wired up via setHost(Host).  Window backend callbacks in
// PlatformDawn forward raw events here.  Input handlers take the platform
// mutex via Host::platformMutex() exactly where the original code did.
class InputController {
public:
    struct Host {
        std::recursive_mutex* platformMutex = nullptr;
        Script::Engine* scriptEngine = nullptr;

        // EventLoop / Window exist only after createTerminal() initializes them;
        // we read them through getters so the same Host survives construction.
        std::function<EventLoop*()> eventLoop;
        std::function<Window*()> window;

        // Active-tab / focused-terminal accessors (caller has locked platformMutex_).
        std::function<std::optional<Tab>()> activeTab;
        std::function<TerminalEmulator*()> activeTerm;

        // Active tab index read / write — for switching tabs via mouse.
        std::function<int()> activeTabIdx;

        // Dispatch a resolved action.  Called under platformMutex_.
        std::function<void(const Action::Any&)> dispatchAction;

        // Redraw + cursor blink reset.
        std::function<void()> setNeedsRedraw;
        std::function<void()> resetBlink;

        // Current framebuffer / padding / cell metrics (caller has lock).
        std::function<uint32_t()> fbWidth;
        std::function<uint32_t()> fbHeight;
        std::function<float()> charWidth;
        std::function<float()> lineHeight;
        std::function<float()> contentScaleX;
        std::function<float()> contentScaleY;
        std::function<float()> padLeft;
        std::function<float()> padTop;

        // Tab bar geometry / visibility for hit-testing.
        std::function<bool()> tabBarVisible;
        std::function<float()> tabBarCharWidth;
        std::function<const std::vector<std::pair<int,int>>&()> tabBarColRanges;

        // Focus change notification into PlatformDawn.
        std::function<void(Tab, int prevId, int newId)> notifyPaneFocusChange;
        std::function<void(int tabIdx)> updateTabTitleFromFocusedPane;

        bool headless = false;
    };

    InputController();
    ~InputController();

    InputController(const InputController&) = delete;
    InputController& operator=(const InputController&) = delete;

    void setHost(Host host) { host_ = std::move(host); }

    // Raw input handlers (called from Window callbacks in PlatformDawn).
    void onKey(int key, int scancode, int action, int mods);
    void onChar(uint32_t codepoint);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    // Bindings management — replaced wholesale on config reload.
    void setKeyBindings(std::vector<Binding> bindings) { bindings_ = std::move(bindings); }
    void setMouseBindings(std::vector<MouseBinding> bindings) { mouseBindings_ = std::move(bindings); }
    void resetSequenceMatcher() { sequenceMatcher_.reset(); }
    // When true (xterm default), Alt+<printable> in legacy keyboard mode sends
    // ESC-prefix + the layout-correct base char instead of whatever the OS's
    // text input system would compose (on macOS this saves e.g. Alt+B from
    // producing "∫" when the user actually wants readline word-nav).
    void setAltSendsEsc(bool v) { altSendsEsc_ = v; }
    // Multi-key sequence idle timeout in ms (0 disables). When a binding is
    // mid-sequence and no key arrives within this window, the matcher is
    // reset and the accumulated prefix keys are resent to the PTY.
    void setKeySequenceTimeoutMs(int ms) { sequenceTimeoutMs_ = ms; }

    // Per-pane cursor style (set from OSC 22 terminal callback, read on every
    // mouse move). Erased when a pane is destroyed.
    void setPaneCursorStyle(int paneId, Window::CursorStyle style) { paneCursorStyle_[paneId] = style; }
    void erasePaneCursorStyle(int paneId) { paneCursorStyle_.erase(paneId); }
    bool hasPaneCursorStyle(int paneId) const { return paneCursorStyle_.find(paneId) != paneCursorStyle_.end(); }
    Window::CursorStyle paneCursorStyle(int paneId) const {
        auto it = paneCursorStyle_.find(paneId);
        return it == paneCursorStyle_.end() ? Window::CursorStyle::Arrow : it->second;
    }

    // Push the active tab's hovered-pane cursor style to the window.
    void refreshPointerShape();

    // Last observed cursor position (unscaled) — used by the scroll-wheel
    // handler in PlatformDawn for reporting mouse coordinates.
    double lastCursorX() const { return lastCursorX_; }
    double lastCursorY() const { return lastCursorY_; }
    uint32_t lastMods() const { return lastMods_; }

    // Resolve which tab index was clicked in the tab bar. Caller holds lock.
    int resolveTabBarClickIndex(double sx, double sy);

    // Map a CSS / kitty pointer name (from OSC 22) to a Window::CursorStyle.
    // Empty string and unknown names fall back to the platform default arrow.
    static Window::CursorStyle pointerShapeNameToCursorStyle(const std::string& name);

private:
    MouseRegion hitTest(double sx, double sy);
    void startAutoScroll(int dir, int col);
    void stopAutoScroll();
    void doAutoScroll();

    Host host_;

    std::vector<Binding> bindings_;
    SequenceMatcher sequenceMatcher_;

    std::vector<MouseBinding> mouseBindings_;
    ClickDetector clickDetector_;

    bool selectionDragActive_ = false;
    bool selectionDragStarted_ = false;
    double selectionDragOriginX_ = 0;
    double selectionDragOriginY_ = 0;

    EventLoop::TimerId autoScrollTimer_ = 0;
    bool autoScrollTimerActive_ = false;
    int autoScrollDir_ = 0;
    int autoScrollCol_ = 0;

    struct MouseContext {
        int cellCol = 0, cellRow = 0;
        int pixelX = 0, pixelY = 0;
        int tabBarClickIndex = -1;
        MouseButton button = MouseButton::Left;
    } mouseCtx_;

    std::unordered_map<int, Window::CursorStyle> paneCursorStyle_;

    bool controlPressed_ = false;
    uint32_t lastMods_ = 0;
    bool altSendsEsc_ = true;

    // Keys swallowed while a multi-key binding is partially matched. Resent
    // to the PTY (via replayPendingSequenceKey) when the sequence aborts
    // on an unmatched follow-up key so prefix keys don't get lost. Each
    // entry is the full onKey arg tuple so we can re-synthesize the
    // shell-bound bytes accurately.
    struct PendingKey { int key; int scancode; int action; int mods; };
    std::vector<PendingKey> pendingSequenceKeys_;
    void replayPendingSequenceKey(const PendingKey& p);

    // Sequence timeout state.
    int      sequenceTimeoutMs_ = 0;
    uint32_t sequenceTimerId_ = 0; // 0 = not running
    void scheduleSequenceTimeout();
    void cancelSequenceTimeout();
    void onSequenceTimeout();
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
    uint32_t heldButtons_ = 0;
};
