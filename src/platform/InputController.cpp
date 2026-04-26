#include "InputController.h"

#include "AnimationScheduler.h"
#include "PlatformDawn.h"
#include "RenderThread.h"
#include "ScriptEngine.h"
#include "Terminal.h"
#include "Utf8.h"

#include <spdlog/spdlog.h>

// Declared in PlatformDawn.h; implemented per-platform.
void platformOpenURL(const std::string& url);

static std::string codepointToUtf8(uint32_t cp) { return utf8::encode(cp); }

static MouseButton buttonToMouseButton(int button) {
    switch (button) {
    case static_cast<int>(LeftButton):   return MouseButton::Left;
    case static_cast<int>(MidButton):    return MouseButton::Middle;
    case static_cast<int>(RightButton):  return MouseButton::Right;
    default: return MouseButton::Left;
    }
}

InputController::InputController() = default;
InputController::~InputController() = default;

Window::CursorStyle InputController::pointerShapeNameToCursorStyle(const std::string& name)
{
    using CS = Window::CursorStyle;
    if (name.empty() || name == "default" || name == "left_ptr" ||
        name == "context-menu" || name == "alias" || name == "copy" ||
        name == "dnd-link" || name == "dnd-copy" || name == "dnd-none" ||
        name == "none")                                              return CS::Arrow;
    if (name == "text" || name == "vertical-text" ||
        name == "xterm" || name == "ibeam")                          return CS::IBeam;
    if (name == "pointer" || name == "pointing_hand" || name == "hand" ||
        name == "hand1" || name == "hand2" || name == "openhand" ||
        name == "closedhand" || name == "grab" || name == "grabbing") return CS::Pointer;
    if (name == "crosshair" || name == "tcross" || name == "cross" ||
        name == "cell" || name == "plus")                            return CS::Crosshair;
    if (name == "wait" || name == "clock" || name == "watch" ||
        name == "progress" || name == "half-busy" ||
        name == "left_ptr_watch")                                    return CS::Wait;
    if (name == "help" || name == "question_arrow" ||
        name == "whats_this")                                        return CS::Help;
    if (name == "move" || name == "fleur" || name == "all-scroll" ||
        name == "pointer-move")                                      return CS::Move;
    if (name == "not-allowed" || name == "no-drop" ||
        name == "forbidden" || name == "crossed_circle" ||
        name == "dnd-no-drop")                                       return CS::NotAllowed;
    if (name == "ew-resize" || name == "e-resize" || name == "w-resize" ||
        name == "col-resize" || name == "right_side" ||
        name == "left_side" || name == "sb_h_double_arrow" ||
        name == "split_h")                                           return CS::ResizeH;
    if (name == "ns-resize" || name == "n-resize" || name == "s-resize" ||
        name == "row-resize" || name == "top_side" ||
        name == "bottom_side" || name == "sb_v_double_arrow" ||
        name == "split_v")                                           return CS::ResizeV;
    if (name == "nesw-resize" || name == "ne-resize" || name == "sw-resize" ||
        name == "top_right_corner" || name == "bottom_left_corner" ||
        name == "size_bdiag" || name == "size-bdiag")                return CS::ResizeNESW;
    if (name == "nwse-resize" || name == "nw-resize" || name == "se-resize" ||
        name == "top_left_corner" || name == "bottom_right_corner" ||
        name == "size_fdiag" || name == "size-fdiag")                return CS::ResizeNWSE;
    if (name == "zoom-in" || name == "zoom_in" ||
        name == "zoom-out" || name == "zoom_out")                    return CS::Crosshair;
    return CS::Arrow;
}

// key, scancode, action, mods are already platform-independent (Key enum, KeyAction enum, Modifier bitmask)
// — the Window backend (XCBWindow/CocoaWindow) does all conversion before calling here.
void InputController::onKey(int key, int scancode, int action, int mods)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    TerminalEmulator* term = static_cast<TerminalEmulator*>(platform_->activeTerm());
    if (!term) return;

    Window* window = platform_->window_.get();

    spdlog::debug("onKey: key=0x{:x} action={} mods={}", key, action, mods);

    controlPressed_ = (mods & CtrlModifier) != 0;
    const uint32_t prevMods = lastMods_;
    lastMods_ = static_cast<uint32_t>(mods);
    if (lastMods_ != prevMods) {
        // Modifier transition (e.g. Ctrl pressed/released) may flip whether a
        // Left+Click on the cell under the cursor would open a hyperlink, so
        // refresh the pointer shape without waiting for mouse motion.
        refreshPointerShape();
    }

    Key k = static_cast<Key>(key);
    spdlog::debug("onKey: key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // Bindings only on press/repeat (NOT release)
    if (action != static_cast<int>(KeyAction_Release)) {
        auto result = sequenceMatcher_.advance({k, lastMods_}, bindings_);
        if (result.result == SequenceMatcher::Result::Match) {
            pendingSequenceKeys_.clear();
            cancelSequenceTimeout();
            for (const auto& act : result.actions)
                platform_->dispatchAction(act);
            return;
        }
        if (result.result == SequenceMatcher::Result::Prefix) {
            pendingSequenceKeys_.push_back({key, scancode, action, mods});
            scheduleSequenceTimeout();
            return;
        }
        // NoMatch: if the sequence had accumulated a prefix, resend those
        // keys to the shell so the user's keystrokes aren't silently lost
        // when they abort a multi-key binding. The current (failing) key
        // falls through to normal processing below.
        cancelSequenceTimeout();
        if (!result.abortedPrefix.empty()) {
            auto pending = std::move(pendingSequenceKeys_);
            pendingSequenceKeys_.clear();
            for (const auto& p : pending) {
                replayPendingSequenceKey(p);
            }
        }
    }

    // OSC 133 highlight: Escape (no modifiers) clears the selected command
    // region and is swallowed. Only takes effect while a command is selected,
    // so normal Escape delivery to TUI apps is preserved at other times.
    if (action != static_cast<int>(KeyAction_Release) &&
        k == Key_Escape && mods == 0 && term->selectedCommandId()) {
        term->setSelectedCommand(std::nullopt);
        return;
    }

    // Esc defocus for embedded terminals. Applets use `q` / Ctrl-C for their
    // own cancel, so Esc is repurposed here as "hand focus back to the
    // parent pane". The focused popup path intentionally forwards Esc to the
    // popup's content; embeddeds differ.
    if (action != static_cast<int>(KeyAction_Release) &&
        k == Key_Escape && mods == 0) {
        auto tab = platform_->activeTab();
        if (tab) {
            Terminal* fp = platform_->scriptEngine_.focusedTerminalInSubtree(*tab);
            if (fp && fp->focusedEmbeddedLineId() != 0) {
                fp->clearFocusedEmbedded();
                fp->focusEvent(true);
                platform_->renderThread_->pending().dirtyPanes.insert(fp->id());
                platform_->setNeedsRedraw();
                return;
            }
        }
    }

    // Reset viewport to live when typing (not on release, not for bindings)
    if (action != static_cast<int>(KeyAction_Release)) {
        term->resetViewport();
        if (platform_->animScheduler_) platform_->animScheduler_->resetBlink();
    }

    KeyEvent ev;
    ev.key = k;
    ev.modifiers = lastMods_;
    ev.action = static_cast<KeyAction>(action);
    ev.autoRepeat = (action == static_cast<int>(KeyAction_Repeat));
    ev.count = 1;

    if (term->kittyFlags() != 0) {
        // Kitty mode: text should be the unmodified key character — modifiers
        // are encoded separately in the CSI u sequence. For ctrl+letter,
        // send the lowercase letter, not the control character.
        if (controlPressed_ && ((key >= Key_A && key <= Key_Z) || (key >= 0x61 && key <= 0x7a))) {
            char ch = (key >= 0x61) ? static_cast<char>(key) : static_cast<char>(key - Key_A + 'a');
            ev.text = std::string(1, ch);
        } else if (key >= Key_Space && key <= Key_AsciiTilde) {
            std::string name = window ? window->keyName(scancode) : std::string{};
            if (!name.empty()) ev.text = name;
        }
        // Populate shifted_key for report_alternate_key mode
        if (window) {
            ev.shiftedKey = window->shiftedKeyCodepoint(scancode);
        }
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy mode: drop release events
    if (action == static_cast<int>(KeyAction_Release)) return;

    // ctrl+letter: generate control character (handle both upper and lower keysyms)
    if (controlPressed_ && ((key >= Key_A && key <= Key_Z) || (key >= 0x61 && key <= 0x7a))) {
        int offset = (key >= 0x61) ? (key - 0x61) : (key - Key_A);
        ev.text = std::string(1, static_cast<char>(offset + 1));
        spdlog::debug("onKey: ctrl+letter, sending text=0x{:02x}", static_cast<unsigned char>(ev.text[0]));
        term->keyPressEvent(&ev);
        return;
    }

    // Alt+printable → ESC prefix (xterm convention, required for readline
    // word-nav like M-b / M-f). We have to handle it here rather than in
    // onChar because on macOS the OS text-input system composes option+letter
    // into a different character (e.g. Alt+B → "∫") by the time onChar fires.
    // Use the window's keyName(scancode), which returns the layout-correct
    // base character regardless of current modifier state, so Dvorak etc.
    // stay correct. The duplicate onChar (from the OS text path on Linux, or
    // from a non-skipped interpretKeyEvents on macOS) is dropped by the
    // AltModifier guard in onChar below.
    if (altSendsEsc_ && (lastMods_ & AltModifier) && !(lastMods_ & CtrlModifier) &&
        window && key < 0x01000000 && k != Key_unknown) {
        std::string base = window->keyName(scancode);
        if (!base.empty() && static_cast<unsigned char>(base[0]) >= 0x20) {
            ev.text = "\x1b" + base;
            term->keyPressEvent(&ev);
            return;
        }
    }

    // Special (non-printable) keys have values >= 0x01000000
    if (k != Key_unknown && key >= 0x01000000) {
        spdlog::debug("onKey: non-printable key=0x{:x}, dispatching", static_cast<int>(k));
        term->keyPressEvent(&ev);
    } else {
        spdlog::debug("onKey: printable key=0x{:x}, deferring to onChar", key);
    }
}


void InputController::onChar(uint32_t codepoint)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    TerminalEmulator* term = static_cast<TerminalEmulator*>(platform_->activeTerm());
    if (!term) return;

    spdlog::debug("onChar: codepoint=U+{:04X} controlPressed={}", codepoint, controlPressed_);
    term->resetViewport();
    if (platform_->animScheduler_) platform_->animScheduler_->resetBlink();

    // In Kitty mode, onKey handles everything
    if (term->kittyFlags() != 0) return;

    if (controlPressed_) return;

    // When alt_sends_esc is on and Alt is held, onKey has already emitted
    // ESC+<base-char>. The OS text path still fires onChar (Linux always;
    // macOS only when skip-IME in Window_cocoa didn't catch it). Drop it
    // so the shell doesn't see the character twice.
    if (altSendsEsc_ && (lastMods_ & AltModifier) && !(lastMods_ & CtrlModifier))
        return;

    KeyEvent ev;
    ev.key = Key_unknown;
    ev.text = codepointToUtf8(codepoint);
    ev.modifiers = lastMods_;
    ev.action = KeyAction_Press;
    spdlog::debug("onChar: sending text='{}' ({} bytes)", ev.text, ev.text.size());
    ev.count = 1;
    ev.autoRepeat = false;
    term->keyPressEvent(&ev);
}


MouseRegion InputController::hitTest(double sx, double sy)
{
    auto tab = platform_->activeTab();
    if (!tab) return MouseRegion::Pane;
    Script::Engine& eng = platform_->scriptEngine_;

    // Check tab bar
    if (platform_->tabBarVisible()) {
        Rect tbRect = eng.tabBarRect(platform_->fbWidth_, platform_->fbHeight_);
        if (!tbRect.isEmpty() &&
            sx >= tbRect.x && sx < tbRect.x + tbRect.w &&
            sy >= tbRect.y && sy < tbRect.y + tbRect.h)
            return MouseRegion::TabBar;
    }

    // Divider hit-test — suppresses pane-selection and terminal forwarding when
    // the click lands on a split divider rather than inside a pane.
    // When zoomed, dividerRects returns nothing: the tree's rect map skips
    // non-zoomed siblings, so no dividers emit from dividersIn. Safe to drop
    // the explicit !isZoomed guard.
    const int divPx = eng.dividerPixels();
    if (divPx > 0) {
        for (const auto& r : eng.tabDividerRects(*tab, divPx)) {
            if (sx >= r.x && sx < r.x + r.w &&
                sy >= r.y && sy < r.y + r.h)
                return MouseRegion::Divider;
        }
    }
    return MouseRegion::Pane;
}

// Resolve which tab index was clicked in the tab bar, given scaled pixel coords.
// Returns -1 if no tab was hit.
int InputController::resolveTabBarClickIndex(double sx, double sy)
{
    float tbCharWidth = platform_->tabBarCharWidth_;
    if (tbCharWidth <= 0.0f) return -1;
    auto tab = platform_->activeTab();
    if (!tab) return -1;
    Rect tbRect = platform_->scriptEngine_.tabBarRect(platform_->fbWidth_, platform_->fbHeight_);
    if (tbRect.isEmpty()) return -1;

    int clickCol = static_cast<int>((sx - tbRect.x) / tbCharWidth);
    const auto& ranges = platform_->tabBarColRanges_;
    for (int i = 0; i < static_cast<int>(ranges.size()); ++i) {
        auto [start, end] = ranges[i];
        if (start < 0) continue; // not visible
        if (clickCol >= start && clickCol < end)
            return i;
    }
    return -1;
}


void InputController::onMouseButton(int button, int action, int mods)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    auto tab = platform_->activeTab();
    if (!tab) return;
    Script::Engine& eng = platform_->scriptEngine_;

    Window* window = platform_->window_.get();

    lastMods_ = static_cast<uint32_t>(mods);

    // Track button state for use in onCursorPos (replaces glfwGetMouseButton polling)
    if (action == static_cast<int>(KeyAction_Press))
        heldButtons_ |= static_cast<uint32_t>(button);
    else
        heldButtons_ &= ~static_cast<uint32_t>(button);

    const float contentScaleX = platform_->contentScaleX_;
    const float contentScaleY = platform_->contentScaleY_;
    const float charWidth = platform_->charWidth_;
    const float lineHeight = platform_->lineHeight_;
    const float padLeft = platform_->padLeft_;
    const float padTop = platform_->padTop_;
    const uint32_t fbWidth = platform_->fbWidth_;
    const uint32_t fbHeight = platform_->fbHeight_;

    double sx = lastCursorX_ * contentScaleX;
    double sy = lastCursorY_ * contentScaleY;

    // Clear selection drag on release and finalize selection
    if (action == static_cast<int>(KeyAction_Release) && selectionDragActive_) {
        selectionDragActive_ = false;
        stopAutoScroll();
        Terminal* fp2 = eng.focusedTerminalInSubtree(*tab);
        TerminalEmulator* term2 = static_cast<TerminalEmulator*>(fp2);
        if (term2) {
            Rect pr = fp2 ? fp2->rect() : Rect{0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight)};
            double cellRelX = sx - pr.x - padLeft;
            double cellRelY = sy - pr.y - padTop;
            MouseEvent ev;
            ev.x = static_cast<int>(cellRelX / charWidth);
            ev.y = static_cast<int>(cellRelY / lineHeight);
            ev.globalX = static_cast<int>(sx);
            ev.globalY = static_cast<int>(sy);
            ev.pixelX = static_cast<int>(cellRelX);
            ev.pixelY = static_cast<int>(cellRelY);
            ev.button = NoButton;
            ev.modifiers = lastMods_;
            term2->mouseReleaseEvent(&ev);
#if defined(__linux__)
            // Publish the completed selection as the X11 primary selection
            if (term2->hasSelection()) {
                std::string sel = term2->selectedText();
                if (!sel.empty())
                    if (window) window->setPrimarySelection(sel);
            }
#endif
        }
        return;
    }

    // 1. Hit test — determine region
    MouseRegion region = hitTest(sx, sy);

    // Divider clicks are not pane input: don't switch focus, don't feed the
    // click detector (would poison subsequent double/triple-click timing),
    // don't start selection, don't forward to terminal mouse reporting.
    if (region == MouseRegion::Divider) return;

    // 2. Click on inactive pane — switch focus (side effect)
    if (action == static_cast<int>(KeyAction_Press) && region == MouseRegion::Pane ) {
        Uuid clickedId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
        Uuid curFocus = eng.focusedPaneInSubtree(*tab);
        if (!clickedId.isNil() && clickedId != curFocus) {
            eng.setFocusedTerminalNodeId(clickedId);
            platform_->notifyPaneFocusChange(*tab, curFocus, clickedId);
            platform_->tabBarDirty_ = true;
            platform_->updateWindowTitle();
            // Cursor outline / divider tint / pane tint all key off the
            // shadow-state focusedPaneId — `applyPendingMutations` rebuilds
            // it on the next tick, but without this kick the render thread
            // has no reason to draw a new frame and the user sees stale
            // focus indication until the next input event.
            platform_->setNeedsRedraw();
        }
    }

    // 2b. Check if click lands inside a popup — deliver mouse event to JS
    if (region == MouseRegion::Pane ) {
        Terminal* clickPane = eng.focusedTerminalInSubtree(*tab);
        if (clickPane) {
            const Rect& pr = clickPane->rect();
            double cellRelX = sx - pr.x - padLeft;
            double cellRelY = sy - pr.y - padTop;
            int cellCol = static_cast<int>(cellRelX / charWidth);
            int cellRow = static_cast<int>(cellRelY / lineHeight);
            for (const auto& popup : clickPane->popups()) {
                if (cellCol >= popup->cellX() && cellCol < popup->cellX() + popup->cellW() &&
                    cellRow >= popup->cellY() && cellRow < popup->cellY() + popup->cellH()) {
                    int relCol = cellCol - popup->cellX();
                    int relRow = cellRow - popup->cellY();
                    int relPixelX = static_cast<int>(cellRelX) - popup->cellX() * static_cast<int>(charWidth);
                    int relPixelY = static_cast<int>(cellRelY) - popup->cellY() * static_cast<int>(lineHeight);
                    std::string popupIdCopy = popup->popupId();

                    // Deliver mouse event to JS popup listeners. The JS handler
                    // may call popup.close() synchronously, which extracts the
                    // popup from clickPane->popups() and invalidates the
                    // reference we're iterating. Don't touch `popup` after
                    // this point — re-resolve by id.
                    std::string type = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
                    int btn = (button == static_cast<int>(LeftButton)) ? 0
                            : (button == static_cast<int>(RightButton)) ? 1 : 2;
                    platform_->scriptEngine_.deliverPopupMouseEvent(
                        clickPane->id(), popupIdCopy, type,
                        relCol, relRow,
                        static_cast<int>(sx), static_cast<int>(sy), btn);
                    platform_->scriptEngine_.executePendingJobs();

                    // VT mouse-reporting forward. If the applet has
                    // DECSET 1000/1002/1006 active inside the popup, its
                    // emulator serializes the click to SGR bytes via
                    // writeToOutput -> onInput -> JS "input" listener.
                    // Skipped when reporting is inactive so we don't arm
                    // a spurious text selection on a headless popup.
                    Terminal* livePopup = nullptr;
                    for (const auto& p : clickPane->popups()) {
                        if (p->popupId() == popupIdCopy) { livePopup = p.get(); break; }
                    }
                    if (livePopup && livePopup->mouseReportingActive()) {
                        MouseEvent mev;
                        mev.x = relCol; mev.y = relRow;
                        mev.globalX = static_cast<int>(sx);
                        mev.globalY = static_cast<int>(sy);
                        mev.pixelX = relPixelX; mev.pixelY = relPixelY;
                        mev.button = static_cast<Button>(button);
                        mev.modifiers = lastMods_;
                        if (action == static_cast<int>(KeyAction_Press))
                            livePopup->mousePressEvent(&mev);
                        else
                            livePopup->mouseReleaseEvent(&mev);
                    }
                    return;
                }
            }

            // 2c. Embedded terminals. Hit-test visual displacement so a click
            // on a visible embedded focuses it. Skipped mid-drag (drag
            // originated outside the embedded — let the selection continue
            // across its rect instead of stealing focus). Also skipped on
            // alt-screen where embeddeds are hidden anyway.
            if (!selectionDragActive_ && !clickPane->usingAltScreen() &&
                !clickPane->embeddeds().empty()) {
                uint64_t hitLineId = 0;
                int emRelCol = 0, emRelRow = 0, emRelPx = 0, emRelPy = 0;
                bool clickedEmbedded = clickPane->liveSegmentHitTest(
                    cellRelX, cellRelY, static_cast<float>(charWidth), lineHeight,
                    hitLineId, emRelCol, emRelRow, emRelPx, emRelPy);
                if (clickedEmbedded) {
                    // On press: focus the embedded (subsequent activeTerm()
                    // resolves to it for keyboard). Always deliver the
                    // mouse event to JS listeners — applets drive their
                    // own widget logic from press + release.
                    if (action == static_cast<int>(KeyAction_Press)) {
                        clickPane->clearFocusedPopup();
                        clickPane->setFocusedEmbeddedLineId(hitLineId);
                        clickPane->focusEvent(false);
                        platform_->renderThread_->pending().dirtyPanes.insert(clickPane->id());
                        platform_->setNeedsRedraw();
                    }
                    std::string type = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
                    int btn = (button == static_cast<int>(LeftButton)) ? 0
                            : (button == static_cast<int>(RightButton)) ? 1 : 2;
                    platform_->scriptEngine_.deliverEmbeddedMouseEvent(
                        clickPane->id(), hitLineId, type,
                        emRelCol, emRelRow, emRelPx, emRelPy, btn);
                    platform_->scriptEngine_.executePendingJobs();

                    // VT mouse-reporting forward for embedded — same as
                    // the popup path. Only fires when DECSET 1000/1002/
                    // 1006 are active, so the classic no-reporting case
                    // doesn't spuriously arm a text selection.
                    if (Terminal* em = clickPane->findEmbedded(hitLineId)) {
                        if (em->mouseReportingActive()) {
                            MouseEvent mev;
                            mev.x = emRelCol; mev.y = emRelRow;
                            mev.globalX = static_cast<int>(sx);
                            mev.globalY = static_cast<int>(sy);
                            mev.pixelX = emRelPx; mev.pixelY = emRelPy;
                            mev.button = static_cast<Button>(button);
                            mev.modifiers = lastMods_;
                            if (action == static_cast<int>(KeyAction_Press))
                                em->mousePressEvent(&mev);
                            else
                                em->mouseReleaseEvent(&mev);
                        }
                    }
                    return;
                }
                // Click outside any embedded: if one is currently focused,
                // defocus it so the parent pane resumes handling input.
                if (action == static_cast<int>(KeyAction_Press) &&
                    clickPane->focusedEmbeddedLineId() != 0) {
                    clickPane->clearFocusedEmbedded();
                    clickPane->focusEvent(true);
                    platform_->renderThread_->pending().dirtyPanes.insert(clickPane->id());
                    platform_->setNeedsRedraw();
                    // Fall through to normal click processing.
                }
            }

            // Deliver pane mouse event to JS (non-consuming — normal flow continues)
            std::string paneEvtType = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
            int paneBtn = (button == static_cast<int>(LeftButton)) ? 0
                        : (button == static_cast<int>(RightButton)) ? 1 : 2;
            platform_->scriptEngine_.deliverPaneMouseEvent(
                clickPane->id(), paneEvtType,
                cellCol, cellRow,
                static_cast<int>(sx), static_cast<int>(sy), paneBtn);
        }
    }

    // 3. Convert GLFW button to MouseButton
    MouseButton mb = buttonToMouseButton(button);

    // 4. Feed to click detector
    ClickDetector::Result clickResult;
    if (action == static_cast<int>(KeyAction_Press)) {
        clickResult = clickDetector_.onPress(mb, static_cast<int>(sx), static_cast<int>(sy));
    } else {
        clickResult = clickDetector_.onRelease(mb, static_cast<int>(sx), static_cast<int>(sy));
    }

    // 5. Resolve focused pane and terminal
    Terminal* fp = eng.focusedTerminalInSubtree(*tab);
    TerminalEmulator* term = static_cast<TerminalEmulator*>(fp);
    if (!term) return;

    // 6. Determine mouse mode
    MouseMode mode = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;

    // 7. Build MouseStroke and match
    MouseStroke stroke;
    stroke.button = mb;
    stroke.mods   = lastMods_;
    stroke.event  = clickResult.type;
    stroke.mode   = mode;
    stroke.region = region;

    auto matched = matchMouseBindings(stroke, mouseBindings_);

    if (!matched.empty()) {
        // Compute cell coordinates relative to pane (subtract padding so col 0
        // begins at the first cell, not the pane edge)
        Rect pr = fp ? fp->rect() : Rect{0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight)};
        double cellRelX = sx - pr.x - padLeft;
        double cellRelY = sy - pr.y - padTop;
        int cellCol = static_cast<int>(cellRelX / charWidth);
        int cellRow = static_cast<int>(cellRelY / lineHeight);

        mouseCtx_.cellCol = cellCol;
        mouseCtx_.cellRow = cellRow;
        mouseCtx_.pixelX  = static_cast<int>(sx);
        mouseCtx_.pixelY  = static_cast<int>(sy);
        mouseCtx_.button  = mb;
        mouseCtx_.tabBarClickIndex = (region == MouseRegion::TabBar)
            ? resolveTabBarClickIndex(sx, sy)
            : -1;

        for (Action::Any& act : matched) {
            // Resolve ActivateTab{-1} / CloseTab{-1} using tab bar click index
            if (auto* at = std::get_if<Action::ActivateTab>(&act)) {
                if (at->index == -1) at->index = mouseCtx_.tabBarClickIndex;
                if (at->index < 0) continue; // no tab hit
            }
            if (auto* ct = std::get_if<Action::CloseTab>(&act)) {
                if (ct->index == -1) ct->index = mouseCtx_.tabBarClickIndex;
                if (ct->index < 0) continue; // no tab hit
            }

            // MouseSelection: dispatch based on selection type
            if (auto* ms = std::get_if<Action::MouseSelection>(&act)) {
                int col = mouseCtx_.cellCol, row = mouseCtx_.cellRow;
                int absRow = term->document().historySize() - term->viewportOffset() + row;

                switch (ms->type) {
                case Action::SelectionType::Normal: {
                    // Arm pending selection via mousePressEvent
                    MouseEvent ev;
                    ev.x = col; ev.y = row;
                    ev.globalX = static_cast<int>(sx);
                    ev.globalY = static_cast<int>(sy);
                    ev.modifiers = lastMods_;
                    ev.button = LeftButton;
                    ev.buttons = ev.button;
                    term->mousePressEvent(&ev);
                    selectionDragActive_ = true;
                    selectionDragOriginX_ = sx;
                    selectionDragOriginY_ = sy;
                    selectionDragStarted_ = false;
                    break;
                }
                case Action::SelectionType::Word:
                    term->startWordSelection(col, absRow);
#if defined(__linux__)
                    if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window) window->setPrimarySelection(s); }
#endif
                    break;
                case Action::SelectionType::Line:
                    term->startLineSelection(absRow);
#if defined(__linux__)
                    if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window) window->setPrimarySelection(s); }
#endif
                    break;
                case Action::SelectionType::Extend:
                    term->extendSelection(col, absRow);
#if defined(__linux__)
                    if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window) window->setPrimarySelection(s); }
#endif
                    break;
                case Action::SelectionType::Rectangle:
                    term->startRectangleSelection(col, absRow);
                    selectionDragActive_ = true;
                    break;
                }
                continue;
            }

            // OpenHyperlink: resolve hyperlink at cell position; no-op when
            // no URL is present. Command selection is a separate action.
            if (std::holds_alternative<Action::OpenHyperlink>(act)) {
                int col = cellCol, row = cellRow;
                if (col >= 0 && col < term->width() && row >= 0 && row < term->height()) {
                    const CellExtra* extra = term->grid().getExtra(col, row);
                    if (extra && extra->hyperlinkId) {
                        const std::string* uri = term->hyperlinkURI(extra->hyperlinkId);
                        if (uri && !uri->empty())
                            platformOpenURL(*uri);
                    }
                }
                continue;
            }

            // SelectCommand: set OSC 133 highlight to the command containing
            // this row (or clear if none). Skipped on alt screen.
            if (std::holds_alternative<Action::SelectCommand>(act)) {
                if (!term->usingAltScreen() && cellRow >= 0 && cellRow < term->height()) {
                    int absRow = term->document().historySize() - term->viewportOffset() + cellRow;
                    uint64_t lineId = term->document().lineIdForAbs(absRow);
                    const auto* rec = term->commandForLineId(lineId);
                    term->setSelectedCommand(rec ? std::optional<uint64_t>{rec->id} : std::nullopt);
                }
                continue;
            }

            // SelectCommandOutput on the mouse path: hit-test the clicked row to
            // find its command and convert the output range into a text
            // selection. Falling through to the generic dispatch would instead
            // use the viewport-center heuristic, which is wrong for a click.
            if (std::holds_alternative<Action::SelectCommandOutput>(act)) {
                if (!term->usingAltScreen() && cellRow >= 0 && cellRow < term->height()) {
                    int absRow = term->document().historySize() - term->viewportOffset() + cellRow;
                    uint64_t lineId = term->document().lineIdForAbs(absRow);
                    const auto* rec = term->commandForLineId(lineId);
                    if (rec) term->selectCommandOutputForRecord(rec);
                }
                continue;
            }

            // PasteSelection: X11 primary selection on Linux, clipboard fallback elsewhere
            if (std::holds_alternative<Action::PasteSelection>(act)) {
                auto* t = dynamic_cast<Terminal*>(term);
                if (t && window) {
                    std::string text = window->getPrimarySelection();
                    if (text.empty()) text = window->getClipboard();
                    if (!text.empty()) t->pasteText(text);
                }
                continue;
            }

            platform_->dispatchAction(act);
        }
        return;
    }

    // No binding match — if grabbed, forward to terminal's mouse reporting.
    // Skip when the click landed on a divider — the pane shouldn't see it.
    if (mode == MouseMode::Grabbed && region == MouseRegion::Pane) {
        Rect pr = fp ? fp->rect() : Rect{0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight)};
        double cellRelX = sx - pr.x - padLeft;
        double cellRelY = sy - pr.y - padTop;

        MouseEvent ev;
        ev.x = static_cast<int>(cellRelX / charWidth);
        ev.y = static_cast<int>(cellRelY / lineHeight);
        ev.globalX = static_cast<int>(sx);
        ev.globalY = static_cast<int>(sy);
        ev.pixelX = static_cast<int>(cellRelX);
        ev.pixelY = static_cast<int>(cellRelY);
        ev.modifiers = lastMods_;
        switch (button) {
        case static_cast<int>(LeftButton):   ev.button = LeftButton; break;
        case static_cast<int>(MidButton):    ev.button = MidButton;  break;
        case static_cast<int>(RightButton):  ev.button = RightButton; break;
        default: ev.button = NoButton; break;
        }
        ev.buttons = ev.button;

        if (action == static_cast<int>(KeyAction_Press)) term->mousePressEvent(&ev);
        else term->mouseReleaseEvent(&ev);
    }
}


void InputController::onCursorPos(double x, double y)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    auto tab = platform_->activeTab();
    if (!tab) return;
    Script::Engine& eng = platform_->scriptEngine_;

    Window* window = platform_->window_.get();
    Terminal* fp = eng.focusedTerminalInSubtree(*tab);
    TerminalEmulator* term = static_cast<TerminalEmulator*>(fp);
    if (!term) return;

    const float contentScaleX = platform_->contentScaleX_;
    const float contentScaleY = platform_->contentScaleY_;
    const float charWidth = platform_->charWidth_;
    const float lineHeight = platform_->lineHeight_;
    const float padLeft = platform_->padLeft_;
    const float padTop = platform_->padTop_;
    const uint32_t fbWidth = platform_->fbWidth_;
    const uint32_t fbHeight = platform_->fbHeight_;

    lastCursorX_ = x;
    lastCursorY_ = y;
    double sx = x * contentScaleX;
    double sy = y * contentScaleY;

    // Update mouse cursor shape based on region. Inside a pane, honour the
    // OSC 22 pointer shape of whichever pane the mouse is physically over
    // (not necessarily the focused one) — matches user intuition that a pane
    // showing a clickable region uses a hand cursor when hovered. Falls back
    // to the I-beam for selection. The tab bar always uses the arrow.
    if (window) {
        MouseRegion region = hitTest(sx, sy);
        if (region == MouseRegion::TabBar) {
            window->setCursorStyle(Window::CursorStyle::Arrow);
        } else if (wouldOpenHyperlinkAt(sx, sy)) {
            window->setCursorStyle(Window::CursorStyle::Pointer);
        } else {
            Uuid hoveredId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx),
                                                      static_cast<int>(sy));
            auto it = paneCursorStyle_.find(hoveredId);
            window->setCursorStyle(it != paneCursorStyle_.end()
                ? it->second
                : Window::CursorStyle::IBeam);
        }
    }

    // Notify JS mousemove listeners for the hovered pane — and, when the
    // cursor is inside a popup or embedded on that pane, for those too.
    // Popup/embedded takes precedence (it's "on top of" the pane visually).
    Uuid hoveredPaneId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    Terminal* hp = hoveredPaneId.isNil() ? nullptr : eng.paneInSubtree(*tab, hoveredPaneId);
    if (hp) {
        Rect hpr = hp->rect();
        double hcx = sx - hpr.x - padLeft;
        double hcy = sy - hpr.y - padTop;

        bool routedToChild = false;
        // Popup first (on top in the z-order).
        if (!routedToChild) {
            int cellCol = static_cast<int>(hcx / charWidth);
            int cellRow = static_cast<int>(hcy / lineHeight);
            for (const auto& popup : hp->popups()) {
                if (cellCol >= popup->cellX() && cellCol < popup->cellX() + popup->cellW() &&
                    cellRow >= popup->cellY() && cellRow < popup->cellY() + popup->cellH()) {
                    int relPxX = static_cast<int>(hcx) - popup->cellX() * static_cast<int>(charWidth);
                    int relPxY = static_cast<int>(hcy) - popup->cellY() * static_cast<int>(lineHeight);
                    platform_->scriptEngine_.deliverPopupMouseMove(
                        hp->id(), popup->popupId(),
                        cellCol - popup->cellX(), cellRow - popup->cellY(),
                        relPxX, relPxY);
                    routedToChild = true;
                    break;
                }
            }
        }
        // Then embedded.
        if (!routedToChild && !hp->usingAltScreen() && !hp->embeddeds().empty()) {
            uint64_t hitLineId = 0;
            int emRelCol = 0, emRelRow = 0, emRelPx = 0, emRelPy = 0;
            if (hp->liveSegmentHitTest(hcx, hcy,
                                       static_cast<float>(charWidth), lineHeight,
                                       hitLineId, emRelCol, emRelRow, emRelPx, emRelPy)) {
                platform_->scriptEngine_.deliverEmbeddedMouseMove(
                    hp->id(), hitLineId, emRelCol, emRelRow, emRelPx, emRelPy);
                routedToChild = true;
            }
        }

        // Pane mousemove still fires regardless — it's the existing
        // contract. Applets that register on both pane and child will
        // see both events; they can filter in JS if needed.
        if (platform_->scriptEngine_.hasPaneMouseMoveListeners(hoveredPaneId)) {
            platform_->scriptEngine_.notifyPaneMouseMove(
                hoveredPaneId,
                static_cast<int>(hcx / charWidth),
                static_cast<int>(hcy / lineHeight),
                static_cast<int>(hcx),
                static_cast<int>(hcy));
        }
    }

    Rect pr = fp ? fp->rect() : Rect{0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight)};
    // relX/relY: pane-relative pixels (used for pane bounds tests).
    // cellRelX/cellRelY: cell-grid-relative pixels (used for cell math + pixel reporting).
    double relX = sx - pr.x;
    double relY = sy - pr.y;
    double cellRelX = relX - padLeft;
    double cellRelY = relY - padTop;

    // Helper to build a MouseEvent from current state
    auto buildMouseEvent = [&]() {
        MouseEvent ev;
        ev.x = static_cast<int>(cellRelX / charWidth);
        ev.y = static_cast<int>(cellRelY / lineHeight);
        ev.globalX = static_cast<int>(sx);
        ev.globalY = static_cast<int>(sy);
        ev.pixelX = static_cast<int>(cellRelX);
        ev.pixelY = static_cast<int>(cellRelY);
        ev.button = NoButton;
        ev.modifiers = lastMods_;
        if ((heldButtons_ & LeftButton) != 0)
            ev.buttons |= LeftButton;
        if ((heldButtons_ & MidButton) != 0)
            ev.buttons |= MidButton;
        if ((heldButtons_ & RightButton) != 0)
            ev.buttons |= RightButton;
        return ev;
    };

    // 1. If selection drag is active, forward directly to terminal
    if (selectionDragActive_) {
        // Require movement beyond a threshold before activating the pending
        // selection, so that subpixel jitter during a click doesn't start one.
        if (!selectionDragStarted_) {
            double dx = sx - selectionDragOriginX_;
            double dy = sy - selectionDragOriginY_;
            if (dx * dx + dy * dy < 9.0) // 3px threshold
                return;
            selectionDragStarted_ = true;
        }
        MouseEvent ev = buildMouseEvent();
        term->mouseMoveEvent(&ev);
        // Clear drag active on release (no buttons held)
        if (ev.buttons == 0) {
            selectionDragActive_ = false;
            stopAutoScroll();
            return;
        }
        // Auto-scroll when mouse is outside the pane's vertical bounds
        double paneH = pr.h;
        if (relY < 0 && term->viewportOffset() < term->document().historySize()) {
            startAutoScroll(+1, ev.x);
        } else if (relY >= paneH && term->viewportOffset() > 0) {
            startAutoScroll(-1, ev.x);
        } else {
            stopAutoScroll();
        }
        return;
    }

    // 2. Check if any button is held
    bool buttonHeld = ((heldButtons_ & LeftButton) != 0)
                   || ((heldButtons_ & MidButton) != 0)
                   || ((heldButtons_ & RightButton) != 0);

    if (buttonHeld) {
        // 3. Feed to click detector — may produce a Drag event
        auto dragResult = clickDetector_.onMove(static_cast<int>(sx), static_cast<int>(sy));
        if (dragResult && dragResult->type == MouseEventType::Drag) {
            MouseMode mode = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;
            MouseRegion region = hitTest(sx, sy);

            MouseStroke stroke;
            stroke.button = dragResult->button;
            stroke.mods   = lastMods_;
            stroke.event  = MouseEventType::Drag;
            stroke.mode   = mode;
            stroke.region = region;

            auto matched = matchMouseBindings(stroke, mouseBindings_);
            if (!matched.empty()) {
                // Drag bindings are rare and overlapping ones on the same
                // stroke don't compose cleanly — use the first match only.
                const Action::Any& act = matched.front();
                if (std::holds_alternative<Action::MouseSelection>(act)) {
                    MouseEvent ev = buildMouseEvent();
                    term->mousePressEvent(&ev);
                    selectionDragActive_ = true;
                } else {
                    platform_->dispatchAction(act);
                }
                return;
            }

            // No match and grabbed — forward to terminal for mouse tracking
            if (mode == MouseMode::Grabbed) {
                MouseEvent ev = buildMouseEvent();
                term->mouseMoveEvent(&ev);
            }
            return;
        }

        // Click detector didn't produce drag yet (below threshold) or already produced it —
        // if grabbed, forward motion for mouse tracking
        if (term->mouseReportingActive()) {
            MouseEvent ev = buildMouseEvent();
            term->mouseMoveEvent(&ev);
        }
    } else {
        // 4. No button held — forward to terminal for motion events (hover/tracking)
        MouseEvent ev = buildMouseEvent();
        term->mouseMoveEvent(&ev);
    }
}

void InputController::startAutoScroll(int dir, int col)
{
    autoScrollDir_ = dir;
    autoScrollCol_ = col;
    if (autoScrollTimerActive_) return; // already running, dir/col updated above
    EventLoop* el = platform_->eventLoop_.get();
    if (!el) return;
    autoScrollTimer_ = el->addTimer(50, true, [this]() { doAutoScroll(); });
    autoScrollTimerActive_ = true;
}

void InputController::stopAutoScroll()
{
    if (!autoScrollTimerActive_) return;
    if (EventLoop* el = platform_->eventLoop_.get())
        el->removeTimer(autoScrollTimer_);
    autoScrollTimerActive_ = false;
}

void InputController::doAutoScroll()
{
    if (!selectionDragActive_) { stopAutoScroll(); return; }

    auto tab = platform_->activeTab();
    if (!tab) { stopAutoScroll(); return; }
    Terminal* fp = platform_->scriptEngine_.focusedTerminalInSubtree(*tab);
    TerminalEmulator* term = static_cast<TerminalEmulator*>(fp);
    if (!term) { stopAutoScroll(); return; }

    term->scrollViewport(autoScrollDir_);

    // Extend selection to the boundary row now visible
    MouseEvent ev;
    ev.x       = autoScrollCol_;
    ev.y       = (autoScrollDir_ > 0) ? 0 : term->height() - 1;
    ev.globalX = 0; ev.globalY = 0;
    ev.button  = NoButton;
    ev.buttons = LeftButton;
    ev.modifiers = lastMods_;
    term->mouseMoveEvent(&ev);

    // Stop if there's no more scrollback to consume
    if (autoScrollDir_ > 0 && term->viewportOffset() >= term->document().historySize())
        stopAutoScroll();
    else if (autoScrollDir_ < 0 && term->viewportOffset() == 0)
        stopAutoScroll();

    platform_->setNeedsRedraw();
}

bool InputController::wouldOpenHyperlinkAt(double sx, double sy)
{
    if (!platform_) return false;
    auto tab = platform_->activeTab();
    if (!tab) return false;
    Script::Engine& eng = platform_->scriptEngine_;
    Uuid hoveredId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    if (hoveredId.isNil()) return false;
    Terminal* hp = eng.paneInSubtree(*tab, hoveredId);
    auto* term = dynamic_cast<TerminalEmulator*>(hp);
    if (!term) return false;

    const float charWidth = platform_->charWidth_;
    const float lineHeight = platform_->lineHeight_;
    const float padLeft = platform_->padLeft_;
    const float padTop = platform_->padTop_;
    if (charWidth <= 0.0f || lineHeight <= 0.0f) return false;

    Rect pr = hp->rect();
    double relX = sx - pr.x - padLeft;
    double relY = sy - pr.y - padTop;
    if (relX < 0 || relY < 0) return false;
    int col = static_cast<int>(relX / charWidth);
    int row = static_cast<int>(relY / lineHeight);
    if (col < 0 || col >= term->width() || row < 0 || row >= term->height()) return false;

    const CellExtra* extra = term->grid().getExtra(col, row);
    if (!extra || !extra->hyperlinkId) return false;
    const std::string* uri = term->hyperlinkURI(extra->hyperlinkId);
    if (!uri || uri->empty()) return false;

    MouseStroke stroke;
    stroke.button = MouseButton::Left;
    stroke.mods = lastMods_;
    stroke.event = MouseEventType::Click;
    stroke.mode = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;
    stroke.region = MouseRegion::Pane;
    auto matched = matchMouseBindings(stroke, mouseBindings_);
    for (const auto& act : matched) {
        if (std::holds_alternative<Action::OpenHyperlink>(act))
            return true;
    }
    return false;
}

void InputController::refreshPointerShape()
{
    Window* window = platform_->window_.get();
    if (!window || platform_->isHeadless()) return;
    auto tab = platform_->activeTab();
    if (!tab) return;
    // When the pointer is over the tab bar, show the arrow regardless of the
    // focused pane's cursor shape — matches the hover path in onMouseMove.
    // Without this, clicking a tab triggers a focus change and applies the
    // focused pane's IBeam even though the pointer is still in the tab bar.
    double sx = lastCursorX_ * platform_->contentScaleX_;
    double sy = lastCursorY_ * platform_->contentScaleY_;
    if (hitTest(sx, sy) == MouseRegion::TabBar) {
        window->setCursorStyle(Window::CursorStyle::Arrow);
        return;
    }
    if (wouldOpenHyperlinkAt(sx, sy)) {
        window->setCursorStyle(Window::CursorStyle::Pointer);
        return;
    }
    // Prefer the pane the mouse is physically over (so split/focus changes
    // don't show a cursor that doesn't match the hovered pane). Falls back to
    // the focused pane when the mouse position isn't usefully hovering one
    // (e.g. before any motion event has fired).
    Script::Engine& eng = platform_->scriptEngine_;
    Uuid paneId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    if (paneId.isNil()) {
        if (Terminal* focPane = eng.focusedTerminalInSubtree(*tab)) paneId = focPane->nodeId();
    }
    auto it = paneCursorStyle_.find(paneId);
    window->setCursorStyle(it != paneCursorStyle_.end()
        ? it->second
        : Window::CursorStyle::IBeam);
}

// Resend a swallowed prefix key to the PTY, matching the same payload onKey
// would have produced for that keystroke (minus the sequence-matcher step).
// Mirrors the branches of onKey but with the legacy printable-key path
// synthesizing text from keyName() — no onChar will follow, so we send
// directly instead of deferring.
void InputController::replayPendingSequenceKey(const PendingKey& p)
{
    TerminalEmulator* term = static_cast<TerminalEmulator*>(platform_->activeTerm());
    if (!term) return;
    Window* window = platform_->window_.get();

    Key k = static_cast<Key>(p.key);
    KeyEvent ev;
    ev.key = k;
    ev.modifiers = static_cast<uint32_t>(p.mods);
    ev.action = KeyAction_Press;
    ev.count = 1;

    const bool ctrl = (p.mods & CtrlModifier) != 0;

    if (term->kittyFlags() != 0) {
        if (ctrl && ((p.key >= Key_A && p.key <= Key_Z) || (p.key >= 0x61 && p.key <= 0x7a))) {
            char ch = (p.key >= 0x61) ? static_cast<char>(p.key) : static_cast<char>(p.key - Key_A + 'a');
            ev.text = std::string(1, ch);
        } else if (p.key >= Key_Space && p.key <= Key_AsciiTilde) {
            std::string name = window ? window->keyName(p.scancode) : std::string{};
            if (!name.empty()) ev.text = name;
        }
        if (window) ev.shiftedKey = window->shiftedKeyCodepoint(p.scancode);
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy: ctrl+letter
    if (ctrl && ((p.key >= Key_A && p.key <= Key_Z) || (p.key >= 0x61 && p.key <= 0x7a))) {
        int offset = (p.key >= 0x61) ? (p.key - 0x61) : (p.key - Key_A);
        ev.text = std::string(1, static_cast<char>(offset + 1));
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy: alt+printable with altSendsEsc
    if (altSendsEsc_ && (p.mods & AltModifier) && !(p.mods & CtrlModifier) &&
        window && p.key < 0x01000000 && k != Key_unknown) {
        std::string base = window->keyName(p.scancode);
        if (!base.empty() && static_cast<unsigned char>(base[0]) >= 0x20) {
            ev.text = "\x1b" + base;
            term->keyPressEvent(&ev);
            return;
        }
    }

    // Legacy: special key (handled by the switch inside keyPressEvent).
    if (k != Key_unknown && p.key >= 0x01000000) {
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy: plain printable. Normal onKey defers to onChar; for replay
    // we synthesize text from keyName so the character reaches the shell.
    if (window) {
        std::string name = window->keyName(p.scancode);
        if (!name.empty()) {
            ev.text = name;
            term->keyPressEvent(&ev);
        }
    }
}

void InputController::scheduleSequenceTimeout()
{
    if (sequenceTimeoutMs_ <= 0) return;
    EventLoop* loop = platform_->eventLoop_.get();
    if (!loop) return;
    cancelSequenceTimeout();
    sequenceTimerId_ = loop->addTimer(
        static_cast<uint64_t>(sequenceTimeoutMs_), false,
        [this]() { onSequenceTimeout(); });
}

void InputController::cancelSequenceTimeout()
{
    if (sequenceTimerId_ == 0) return;
    if (EventLoop* loop = platform_->eventLoop_.get()) {
        loop->removeTimer(sequenceTimerId_);
    }
    sequenceTimerId_ = 0;
}

void InputController::onSequenceTimeout()
{
    // The timer fires on the main/event-loop thread, same as onKey, so we
    // take the platform mutex to serialize with any in-flight input.
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    sequenceTimerId_ = 0;
    if (pendingSequenceKeys_.empty()) {
        sequenceMatcher_.reset();
        return;
    }
    auto pending = std::move(pendingSequenceKeys_);
    pendingSequenceKeys_.clear();
    sequenceMatcher_.reset();
    for (const auto& p : pending) {
        replayPendingSequenceKey(p);
    }
}
