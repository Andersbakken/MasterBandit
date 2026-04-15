#include "PlatformDawn.h"
#include "Utf8.h"
#include "Utils.h"
#include <sys/ioctl.h>

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }
static std::string codepointToUtf8(uint32_t cp) { return utf8::encode(cp); }


// key, scancode, action, mods are already platform-independent (Key enum, KeyAction enum, Modifier bitmask)
// — the Window backend (XCBWindow/CocoaWindow) does all conversion before calling here.
void PlatformDawn::onKey(int key, int scancode, int action, int mods)
{
    std::lock_guard<std::mutex> plk(platformMutex_);
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onKey: key=0x{:x} action={} mods={}", key, action, mods);

    controlPressed_ = (mods & CtrlModifier) != 0;
    lastMods_ = static_cast<uint32_t>(mods);

    Key k = static_cast<Key>(key);
    spdlog::debug("onKey: key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // Bindings only on press/repeat (NOT release)
    if (action != static_cast<int>(KeyAction_Release)) {
        auto result = sequenceMatcher_.advance({k, lastMods_}, bindings_);
        if (result.result == SequenceMatcher::Result::Match) {
            dispatchAction(*result.action);
            return;
        }
        if (result.result == SequenceMatcher::Result::Prefix) {
            return;
        }
    }

    // Reset viewport to live when typing (not on release, not for bindings)
    if (action != static_cast<int>(KeyAction_Release)) {
        term->resetViewport();
        resetCursorBlink();
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
            std::string name = window_ ? window_->keyName(scancode) : std::string{};
            if (!name.empty()) ev.text = name;
        }
        // Populate shifted_key for report_alternate_key mode
        if (window_) {
            ev.shiftedKey = window_->shiftedKeyCodepoint(scancode);
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

    // Special (non-printable) keys have values >= 0x01000000
    if (k != Key_unknown && key >= 0x01000000) {
        spdlog::debug("onKey: non-printable key=0x{:x}, dispatching", static_cast<int>(k));
        term->keyPressEvent(&ev);
    } else {
        spdlog::debug("onKey: printable key=0x{:x}, deferring to onChar", key);
    }
}


void PlatformDawn::onChar(uint32_t codepoint)
{
    std::lock_guard<std::mutex> plk(platformMutex_);
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onChar: codepoint=U+{:04X} controlPressed={}", codepoint, controlPressed_);
    term->resetViewport();
    resetCursorBlink();

    // In Kitty mode, onKey handles everything
    if (term->kittyFlags() != 0) return;

    if (controlPressed_) return;

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


void PlatformDawn::adjustFontSize(float delta)
{
    float newSize;
    if (delta == 0.0f) {
        newSize = baseFontSize_; // reset
    } else {
        newSize = fontSize_ + delta * contentScaleX_;
    }
    if (newSize < 6.0f * contentScaleX_ || newSize > 72.0f * contentScaleX_) return;
    fontSize_ = newSize;

    // Recalculate metrics
    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) return;
    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

    // Trigger resize of all panes (recalculates grid dimensions)
    onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
}


void PlatformDawn::onFramebufferResize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> plk(platformMutex_);

    // During a live window drag, debounce the full resize work (surface
    // reconfigure + layout recompute + pane/terminal reflow) to 25 ms.
    // One-off resizes (first show, programmatic, live-resize end) apply
    // immediately.
    const bool live = window_ && window_->inLiveResize();
    if (live) {
        pendingResizeW_ = static_cast<uint32_t>(width);
        pendingResizeH_ = static_cast<uint32_t>(height);
        if (eventLoop_ && resizeDebounceTimer_ == 0) {
            resizeDebounceTimer_ = eventLoop_->addTimer(25, false, [this]() {
                // Lightweight update during live drag: signal the render thread
                // to reconfigure the surface and release stale textures.
                // The heavy work (terminal reflow, GPU buffer updates, dividers)
                // is deferred to onLiveResizeEnd → flushPendingFramebufferResize
                // so platformMutex_ is held only briefly here, keeping XSync
                // counter acknowledgments prompt.
                bool didResize = false;
                {
                    std::lock_guard<std::mutex> plk(platformMutex_);
                    resizeDebounceTimer_ = 0;
                    if (pendingResizeW_ && pendingResizeH_) {
                        fbWidth_  = pendingResizeW_;
                        fbHeight_ = pendingResizeH_;
                        // Leave pendingResizeW_/H_ set so flushPendingFramebufferResize
                        // can pass the final size to applyFramebufferResize.
                        surfaceNeedsReconfigure_.store(true, std::memory_order_release);
                        renderer_.setViewportSize(fbWidth_, fbHeight_);
                        for (auto& [id, rs] : paneRenderStates_) {
                            if (rs.heldTexture) {
                                rs.pendingRelease.push_back(rs.heldTexture);
                                rs.heldTexture = nullptr;
                            }
                            rs.dirty = true;
                        }
                        if (tabBarTexture_) {
                            pendingTabBarRelease_.push_back(tabBarTexture_);
                            tabBarTexture_ = nullptr;
                        }
                        tabBarDirty_ = true;
                        didResize = true;
                    }
                }
                if (didResize) {
                    setNeedsRedraw();
                    wakeRenderThread();
                }
            });
        }
        // Even while debounced, update fbWidth_/fbHeight_ so other main-thread
        // readers (tab bar layout, hit-testing) see the latest window size.
        // The actual surface reconfigure + reflow happens when the timer fires.
        fbWidth_  = static_cast<uint32_t>(width);
        fbHeight_ = static_cast<uint32_t>(height);
        setNeedsRedraw();
        return;
    }

    applyFramebufferResize(width, height);
}

void PlatformDawn::flushPendingFramebufferResize()
{
    // Called on main thread under platformMutex_ from onLiveResizeEnd.
    if (resizeDebounceTimer_ && eventLoop_) {
        eventLoop_->removeTimer(resizeDebounceTimer_);
        resizeDebounceTimer_ = 0;
    }
    if (pendingResizeW_ && pendingResizeH_) {
        applyFramebufferResize(static_cast<int>(pendingResizeW_),
                               static_cast<int>(pendingResizeH_));
        pendingResizeW_ = pendingResizeH_ = 0;
    }
}

void PlatformDawn::applyFramebufferResize(int width, int height)
{
    // Caller must hold platformMutex_.
    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    surfaceNeedsReconfigure_.store(true, std::memory_order_release);
    renderer_.setViewportSize(fbWidth_, fbHeight_);
    renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_);

    // Clear divider buffers for all tabs — geometry is now stale
    for (auto& tabPtr : tabs_)
        clearDividers(tabPtr.get());

    for (auto& tabPtr : tabs_) {
        tabPtr->layout()->computeRects(fbWidth_, fbHeight_);

        for (auto& panePtr : tabPtr->layout()->panes()) {
            Pane* pane = panePtr.get();
            pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

            TerminalEmulator* term = pane->terminal();
            if (!term) continue;

            int cols = term->width();
            int rows = term->height();
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;

            scriptEngine_.notifyPaneResized(pane->id(), cols, rows);

            auto& rs = paneRenderStates_[pane->id()];
            rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
        }
    }

    // Release all held textures — they're now the wrong size for the new framebuffer.
    for (auto& [id, rs] : paneRenderStates_) {
        if (rs.heldTexture) {
            rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
        }
        rs.dirty = true;
    }
    if (tabBarTexture_) {
        pendingTabBarRelease_.push_back(tabBarTexture_);
        tabBarTexture_ = nullptr;
    }
    tabBarDirty_ = true;
    if (Tab* active = activeTab()) refreshDividers(active);
    setNeedsRedraw();
}


// --- Mouse binding helpers ---

static MouseButton buttonToMouseButton(int button) {
    switch (button) {
    case static_cast<int>(LeftButton):   return MouseButton::Left;
    case static_cast<int>(MidButton): return MouseButton::Middle;
    case static_cast<int>(RightButton):  return MouseButton::Right;
    default: return MouseButton::Left;
    }
}

MouseRegion PlatformDawn::hitTest(double sx, double sy)
{
    Tab* tab = activeTab();
    if (!tab) return MouseRegion::Pane;

    // Check tab bar
    if (tabBarVisible()) {
        PaneRect tbRect = tab->layout()->tabBarRect(fbWidth_, fbHeight_);
        if (!tbRect.isEmpty() &&
            sx >= tbRect.x && sx < tbRect.x + tbRect.w &&
            sy >= tbRect.y && sy < tbRect.y + tbRect.h)
            return MouseRegion::TabBar;
    }

    // TODO: divider hit-test
    return MouseRegion::Pane;
}

// Resolve which tab index was clicked in the tab bar, given scaled pixel coords.
// Returns -1 if no tab was hit.
int PlatformDawn::resolveTabBarClickIndex(double sx, double sy)
{
    if (tabBarCharWidth_ <= 0.0f) return -1;
    Tab* tab = activeTab();
    if (!tab) return -1;
    PaneRect tbRect = tab->layout()->tabBarRect(fbWidth_, fbHeight_);
    if (tbRect.isEmpty()) return -1;

    int clickCol = static_cast<int>((sx - tbRect.x) / tabBarCharWidth_);
    for (int i = 0; i < static_cast<int>(tabBarColRanges_.size()); ++i) {
        auto [start, end] = tabBarColRanges_[i];
        if (start < 0) continue; // not visible
        if (clickCol >= start && clickCol < end)
            return i;
    }
    return -1;
}


void PlatformDawn::onMouseButton(int button, int action, int mods)
{
    std::lock_guard<std::mutex> plk(platformMutex_);
    Tab* tab = activeTab();
    if (!tab) return;

    lastMods_ = static_cast<uint32_t>(mods);

    // Track button state for use in onCursorPos (replaces glfwGetMouseButton polling)
    if (action == static_cast<int>(KeyAction_Press))
        heldButtons_ |= static_cast<uint32_t>(button);
    else
        heldButtons_ &= ~static_cast<uint32_t>(button);

    double sx = lastCursorX_ * contentScaleX_;
    double sy = lastCursorY_ * contentScaleY_;

    // Clear selection drag on release and finalize selection
    if (action == static_cast<int>(KeyAction_Release) && selectionDragActive_) {
        selectionDragActive_ = false;
        stopAutoScroll();
        Pane* fp2 = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
        TerminalEmulator* term2 = tab->hasOverlay()
            ? static_cast<TerminalEmulator*>(tab->topOverlay())
            : (fp2 ? static_cast<TerminalEmulator*>(fp2->terminal()) : nullptr);
        if (term2) {
            PaneRect pr = fp2 ? fp2->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
            MouseEvent ev;
            ev.x = static_cast<int>((sx - pr.x) / charWidth_);
            ev.y = static_cast<int>((sy - pr.y) / lineHeight_);
            ev.globalX = static_cast<int>(sx);
            ev.globalY = static_cast<int>(sy);
            ev.pixelX = static_cast<int>(sx - pr.x);
            ev.pixelY = static_cast<int>(sy - pr.y);
            ev.button = NoButton;
            ev.modifiers = lastMods_;
            term2->mouseReleaseEvent(&ev);
#if defined(__linux__)
            // Publish the completed selection as the X11 primary selection
            if (term2->hasSelection()) {
                std::string sel = term2->selectedText();
                if (!sel.empty())
                    if (window_) window_->setPrimarySelection(sel);
            }
#endif
        }
        return;
    }

    // 1. Hit test — determine region
    MouseRegion region = hitTest(sx, sy);

    // 2. Click on inactive pane — switch focus (side effect)
    if (action == static_cast<int>(KeyAction_Press) && region == MouseRegion::Pane && !tab->hasOverlay()) {
        int clickedId = tab->layout()->paneAtPixel(static_cast<int>(sx), static_cast<int>(sy));
        if (clickedId >= 0 && clickedId != tab->layout()->focusedPaneId()) {
            int prev = tab->layout()->focusedPaneId();
            tab->layout()->setFocusedPane(clickedId);
            notifyPaneFocusChange(tab, prev, clickedId);
            updateTabTitleFromFocusedPane(activeTabIdx_);
        }
    }

    // 2b. Check if click lands inside a popup — deliver mouse event to JS
    if (region == MouseRegion::Pane && !tab->hasOverlay()) {
        Pane* clickPane = tab->layout()->focusedPane();
        if (clickPane) {
            const PaneRect& pr = clickPane->rect();
            int cellCol = static_cast<int>((sx - pr.x - padLeft_) / charWidth_);
            int cellRow = static_cast<int>((sy - pr.y - padTop_) / lineHeight_);
            for (const auto& popup : clickPane->popups()) {
                if (cellCol >= popup.cellX && cellCol < popup.cellX + popup.cellW &&
                    cellRow >= popup.cellY && cellRow < popup.cellY + popup.cellH) {
                    int relCol = cellCol - popup.cellX;
                    int relRow = cellRow - popup.cellY;

                    // Deliver mouse event to JS popup listeners
                    std::string type = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
                    int btn = (button == static_cast<int>(LeftButton)) ? 0
                            : (button == static_cast<int>(RightButton)) ? 1 : 2;
                    scriptEngine_.deliverPopupMouseEvent(
                        clickPane->id(), popup.id, type,
                        relCol, relRow,
                        static_cast<int>(sx), static_cast<int>(sy), btn);
                    scriptEngine_.executePendingJobs();
                    return;
                }
            }

            // Deliver pane mouse event to JS (non-consuming — normal flow continues)
            std::string paneEvtType = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
            int paneBtn = (button == static_cast<int>(LeftButton)) ? 0
                        : (button == static_cast<int>(RightButton)) ? 1 : 2;
            scriptEngine_.deliverPaneMouseEvent(
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
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? static_cast<TerminalEmulator*>(fp->terminal()) : nullptr);
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

    auto matched = matchMouseBinding(stroke, mouseBindings_);

    if (matched) {
        // Compute cell coordinates relative to pane
        PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
        double relX = sx - pr.x;
        double relY = sy - pr.y;
        int cellCol = static_cast<int>(relX / charWidth_);
        int cellRow = static_cast<int>(relY / lineHeight_);

        // Populate mouseCtx_ before dispatching
        mouseCtx_.cellCol = cellCol;
        mouseCtx_.cellRow = cellRow;
        mouseCtx_.pixelX  = static_cast<int>(sx);
        mouseCtx_.pixelY  = static_cast<int>(sy);
        mouseCtx_.button  = mb;
        mouseCtx_.tabBarClickIndex = (region == MouseRegion::TabBar)
            ? resolveTabBarClickIndex(sx, sy)
            : -1;

        Action::Any act = *matched;

        // Resolve ActivateTab{-1} / CloseTab{-1} using tab bar click index
        if (auto* at = std::get_if<Action::ActivateTab>(&act)) {
            if (at->index == -1) at->index = mouseCtx_.tabBarClickIndex;
            if (at->index < 0) return; // no tab hit
        }
        if (auto* ct = std::get_if<Action::CloseTab>(&act)) {
            if (ct->index == -1) ct->index = mouseCtx_.tabBarClickIndex;
            if (ct->index < 0) return; // no tab hit
        }

        // MouseSelection: dispatch based on selection type
        if (auto* ms = std::get_if<Action::MouseSelection>(&act)) {
            int col = mouseCtx_.cellCol, row = mouseCtx_.cellRow;
            int absRow = term->document().historySize() - term->viewportOffset() + row;

            switch (ms->type) {
            case Action::SelectionType::Normal: {
                // Arm pending selection via mousePressEvent
                PaneRect pr2 = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
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
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window_) window_->setPrimarySelection(s); }
#endif
                break;
            case Action::SelectionType::Line:
                term->startLineSelection(absRow);
#if defined(__linux__)
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window_) window_->setPrimarySelection(s); }
#endif
                break;
            case Action::SelectionType::Extend:
                term->extendSelection(col, absRow);
#if defined(__linux__)
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) if (window_) window_->setPrimarySelection(s); }
#endif
                break;
            case Action::SelectionType::Rectangle:
                term->startRectangleSelection(col, absRow);
                selectionDragActive_ = true;
                break;
            }
        }

        // OpenHyperlink: resolve hyperlink at cell position
        if (std::holds_alternative<Action::OpenHyperlink>(act)) {
            int col = cellCol, row = cellRow;
            if (col >= 0 && col < term->width() && row >= 0 && row < term->height()) {
                const CellExtra* extra = term->grid().getExtra(col, row);
                if (extra && extra->hyperlinkId) {
                    const std::string* uri = term->hyperlinkURI(extra->hyperlinkId);
                    if (uri && !uri->empty()) {
                        platformOpenURL(*uri);
                        return;
                    }
                }
            }
            return; // no hyperlink found, nothing to do
        }

        // PasteSelection: X11 primary selection on Linux, clipboard fallback elsewhere
        if (std::holds_alternative<Action::PasteSelection>(act)) {
            auto* t = dynamic_cast<Terminal*>(term);
            if (t && window_) {
                // Primary selection on Linux, clipboard elsewhere
                std::string text = window_->getPrimarySelection();
                if (text.empty()) text = window_->getClipboard();
                if (!text.empty()) t->pasteText(text);
            }
            return;
        }

        dispatchAction(act);
        return;
    }

    // No binding match — if grabbed, forward to terminal's mouse reporting
    if (mode == MouseMode::Grabbed) {
        PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
        double relX = sx - pr.x;
        double relY = sy - pr.y;

        MouseEvent ev;
        ev.x = static_cast<int>(relX / charWidth_);
        ev.y = static_cast<int>(relY / lineHeight_);
        ev.globalX = static_cast<int>(sx);
        ev.globalY = static_cast<int>(sy);
        ev.pixelX = static_cast<int>(relX);
        ev.pixelY = static_cast<int>(relY);
        ev.modifiers = lastMods_;
        switch (button) {
        case static_cast<int>(LeftButton):   ev.button = LeftButton; break;
        case static_cast<int>(MidButton): ev.button = MidButton;  break;
        case static_cast<int>(RightButton):  ev.button = RightButton; break;
        default: ev.button = NoButton; break;
        }
        ev.buttons = ev.button;

        if (action == static_cast<int>(KeyAction_Press)) term->mousePressEvent(&ev);
        else term->mouseReleaseEvent(&ev);
    }
}


void PlatformDawn::onCursorPos(double x, double y)
{
    std::lock_guard<std::mutex> plk(platformMutex_);
    Tab* tab = activeTab();
    if (!tab) return;
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? static_cast<TerminalEmulator*>(fp->terminal()) : nullptr);
    if (!term) return;

    lastCursorX_ = x;
    lastCursorY_ = y;
    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    // Update mouse cursor shape based on region. Inside a pane, honour the
    // OSC 22 pointer shape of whichever pane the mouse is physically over
    // (not necessarily the focused one) — matches user intuition that a pane
    // showing a clickable region uses a hand cursor when hovered. Falls back
    // to the I-beam for selection. The tab bar always uses the arrow. Overlays
    // cover the whole tab and aren't tracked in paneCursorStyle_, so they get
    // IBeam by default.
    if (window_) {
        MouseRegion region = hitTest(sx, sy);
        if (region == MouseRegion::TabBar) {
            window_->setCursorStyle(Window::CursorStyle::Arrow);
        } else {
            int hoveredId = tab->hasOverlay()
                ? -1
                : tab->layout()->paneAtPixel(static_cast<int>(sx),
                                             static_cast<int>(sy));
            auto it = paneCursorStyle_.find(hoveredId);
            window_->setCursorStyle(it != paneCursorStyle_.end()
                ? it->second
                : Window::CursorStyle::IBeam);
        }
    }

    PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
    double relX = sx - pr.x;
    double relY = sy - pr.y;

    // Helper to build a MouseEvent from current state
    auto buildMouseEvent = [&]() {
        MouseEvent ev;
        ev.x = static_cast<int>(relX / charWidth_);
        ev.y = static_cast<int>(relY / lineHeight_);
        ev.globalX = static_cast<int>(sx);
        ev.globalY = static_cast<int>(sy);
        ev.pixelX = static_cast<int>(relX);
        ev.pixelY = static_cast<int>(relY);
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

            auto matched = matchMouseBinding(stroke, mouseBindings_);
            if (matched) {
                // For drag bindings (e.g. MouseSelection with Drag),
                // arm selection and let terminal handle subsequent moves
                if (std::holds_alternative<Action::MouseSelection>(*matched)) {
                    MouseEvent ev = buildMouseEvent();
                    term->mousePressEvent(&ev);
                    selectionDragActive_ = true;
                } else {
                    dispatchAction(*matched);
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

void PlatformDawn::startAutoScroll(int dir, int col)
{
    autoScrollDir_ = dir;
    autoScrollCol_ = col;
    if (autoScrollTimerActive_) return; // already running, dir/col updated above
    autoScrollTimer_ = eventLoop_->addTimer(50, true, [this]() { doAutoScroll(); });
    autoScrollTimerActive_ = true;
}

void PlatformDawn::stopAutoScroll()
{
    if (!autoScrollTimerActive_) return;
    eventLoop_->removeTimer(autoScrollTimer_);
    autoScrollTimerActive_ = false;
}

void PlatformDawn::doAutoScroll()
{
    if (!selectionDragActive_) { stopAutoScroll(); return; }

    Tab* tab = activeTab();
    if (!tab) { stopAutoScroll(); return; }
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : [&]() -> TerminalEmulator* {
            Pane* fp = tab->layout()->focusedPane();
            return fp ? static_cast<TerminalEmulator*>(fp->terminal()) : nullptr;
          }();
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

    setNeedsRedraw();
}

