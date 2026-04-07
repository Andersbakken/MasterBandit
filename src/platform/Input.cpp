#include "PlatformDawn.h"
#include "Utf8.h"
#include "Utils.h"
#include <sys/ioctl.h>

#if defined(__linux__)
#  define GLFW_EXPOSE_NATIVE_X11
#  include <GLFW/glfw3native.h>
#endif

// --- GLFW modifier conversion ---

static unsigned int glfwModsToModifiers(int mods)
{
    unsigned int m = 0;
    if (mods & GLFW_MOD_SHIFT) m |= ShiftModifier;
    if (mods & GLFW_MOD_CONTROL) m |= CtrlModifier;
    if (mods & GLFW_MOD_ALT) m |= AltModifier;
    if (mods & GLFW_MOD_SUPER) m |= MetaModifier;
    if (mods & GLFW_MOD_CAPS_LOCK) m |= CapsLockModifier;
    if (mods & GLFW_MOD_NUM_LOCK) m |= NumLockModifier;
    return m;
}

// --- GLFW key → Platform Key mapping ---

static Key glfwKeyToKey(int glfwKey)
{
    switch (glfwKey) {
    case GLFW_KEY_ESCAPE:       return Key_Escape;
    case GLFW_KEY_TAB:          return Key_Tab;
    case GLFW_KEY_BACKSPACE:    return Key_Backspace;
    case GLFW_KEY_ENTER:        return Key_Return;
    case GLFW_KEY_KP_ENTER:     return Key_Enter;
    case GLFW_KEY_INSERT:       return Key_Insert;
    case GLFW_KEY_DELETE:        return Key_Delete;
    case GLFW_KEY_PAUSE:        return Key_Pause;
    case GLFW_KEY_PRINT_SCREEN: return Key_Print;
    case GLFW_KEY_HOME:         return Key_Home;
    case GLFW_KEY_END:          return Key_End;
    case GLFW_KEY_LEFT:         return Key_Left;
    case GLFW_KEY_UP:           return Key_Up;
    case GLFW_KEY_RIGHT:        return Key_Right;
    case GLFW_KEY_DOWN:         return Key_Down;
    case GLFW_KEY_PAGE_UP:      return Key_PageUp;
    case GLFW_KEY_PAGE_DOWN:    return Key_PageDown;
    case GLFW_KEY_LEFT_SHIFT:   return Key_Shift_L;
    case GLFW_KEY_RIGHT_SHIFT:  return Key_Shift_R;
    case GLFW_KEY_LEFT_CONTROL: return Key_Control_L;
    case GLFW_KEY_RIGHT_CONTROL: return Key_Control_R;
    case GLFW_KEY_LEFT_ALT:     return Key_Alt_L;
    case GLFW_KEY_RIGHT_ALT:    return Key_Alt_R;
    case GLFW_KEY_LEFT_SUPER:   return Key_Super_L;
    case GLFW_KEY_RIGHT_SUPER:  return Key_Super_R;
    case GLFW_KEY_KP_0:         return Key_KP_0;
    case GLFW_KEY_KP_1:         return Key_KP_1;
    case GLFW_KEY_KP_2:         return Key_KP_2;
    case GLFW_KEY_KP_3:         return Key_KP_3;
    case GLFW_KEY_KP_4:         return Key_KP_4;
    case GLFW_KEY_KP_5:         return Key_KP_5;
    case GLFW_KEY_KP_6:         return Key_KP_6;
    case GLFW_KEY_KP_7:         return Key_KP_7;
    case GLFW_KEY_KP_8:         return Key_KP_8;
    case GLFW_KEY_KP_9:         return Key_KP_9;
    case GLFW_KEY_KP_DECIMAL:   return Key_KP_Decimal;
    case GLFW_KEY_KP_DIVIDE:    return Key_KP_Divide;
    case GLFW_KEY_KP_MULTIPLY:  return Key_KP_Multiply;
    case GLFW_KEY_KP_SUBTRACT:  return Key_KP_Subtract;
    case GLFW_KEY_KP_ADD:       return Key_KP_Add;
    case GLFW_KEY_KP_EQUAL:     return Key_KP_Equal;
    case GLFW_KEY_CAPS_LOCK:    return Key_CapsLock;
    case GLFW_KEY_NUM_LOCK:     return Key_NumLock;
    case GLFW_KEY_SCROLL_LOCK:  return Key_ScrollLock;
    case GLFW_KEY_F1:           return Key_F1;
    case GLFW_KEY_F2:           return Key_F2;
    case GLFW_KEY_F3:           return Key_F3;
    case GLFW_KEY_F4:           return Key_F4;
    case GLFW_KEY_F5:           return Key_F5;
    case GLFW_KEY_F6:           return Key_F6;
    case GLFW_KEY_F7:           return Key_F7;
    case GLFW_KEY_F8:           return Key_F8;
    case GLFW_KEY_F9:           return Key_F9;
    case GLFW_KEY_F10:          return Key_F10;
    case GLFW_KEY_F11:          return Key_F11;
    case GLFW_KEY_F12:          return Key_F12;
    case GLFW_KEY_SPACE:        return Key_Space;
    case GLFW_KEY_MENU:         return Key_Menu;
    default:
        if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z)
            return static_cast<Key>(Key_A + (glfwKey - GLFW_KEY_A));
        if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9)
            return static_cast<Key>(Key_0 + (glfwKey - GLFW_KEY_0));
        return Key_unknown;
    }
}

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }
static std::string codepointToUtf8(uint32_t cp) { return utf8::encode(cp); }


void PlatformDawn::onKey(int key, int scancode, int action, int mods)
{
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onKey: key={} action={} mods={}", key, action, mods);

    controlPressed_ = (mods & GLFW_MOD_CONTROL) != 0;
    lastMods_ = glfwModsToModifiers(mods);

    Key k = glfwKeyToKey(key);
    spdlog::debug("onKey: mapped key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // Bindings only on press/repeat (NOT release)
    if (action != GLFW_RELEASE) {
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
    if (action != GLFW_RELEASE) {
        term->resetViewport();
    }

    KeyEvent ev;
    ev.key = k;
    ev.modifiers = lastMods_;
    ev.action = (action == GLFW_RELEASE) ? KeyAction_Release
              : (action == GLFW_REPEAT)  ? KeyAction_Repeat
              :                            KeyAction_Press;
    ev.autoRepeat = (action == GLFW_REPEAT);
    ev.count = 1;

    if (term->kittyFlags() != 0) {
        // Kitty mode: generate text from key for printable keys
        if (key >= GLFW_KEY_SPACE && key <= GLFW_KEY_GRAVE_ACCENT) {
            const char* name = glfwGetKeyName(key, scancode);
            if (name) ev.text = name;
        } else if (controlPressed_ && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
            ev.text = std::string(1, static_cast<char>(key - GLFW_KEY_A + 1));
        }
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy mode: drop release events
    if (action == GLFW_RELEASE) return;

    // ctrl+letter: generate control character
    if (controlPressed_ && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        ev.text = std::string(1, static_cast<char>(key - GLFW_KEY_A + 1));
        spdlog::debug("onKey: ctrl+letter, sending text=0x{:02x}", static_cast<unsigned char>(ev.text[0]));
        term->keyPressEvent(&ev);
        return;
    }

    if (k != Key_unknown && (key < GLFW_KEY_SPACE || key > GLFW_KEY_GRAVE_ACCENT)) {
        spdlog::debug("onKey: non-printable key=0x{:x}, dispatching", static_cast<int>(k));
        term->keyPressEvent(&ev);
    } else {
        spdlog::debug("onKey: printable key={}, deferring to onChar", key);
    }
}


void PlatformDawn::onChar(unsigned int codepoint)
{
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onChar: codepoint=U+{:04X} controlPressed={}", codepoint, controlPressed_);
    term->resetViewport();

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

    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    configureSurface(fbWidth_, fbHeight_);
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
    needsRedraw_ = true;

}


// --- Mouse binding helpers ---

static MouseButton glfwButtonToMouseButton(int button) {
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:   return MouseButton::Left;
    case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
    case GLFW_MOUSE_BUTTON_RIGHT:  return MouseButton::Right;
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
static int resolveTabBarClickIndex(double sx, double sy,
                                   float tabBarCharWidth,
                                   uint32_t fbWidth, uint32_t fbHeight,
                                   Tab* tab,
                                   const std::vector<std::unique_ptr<Tab>>& tabs)
{
    if (tabBarCharWidth <= 0.0f) return -1;
    PaneRect tbRect = tab->layout()->tabBarRect(fbWidth, fbHeight);
    if (tbRect.isEmpty()) return -1;

    int clickCol = static_cast<int>((sx - tbRect.x) / tabBarCharWidth);
    int col = 0;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        Tab* t = tabs[i].get();
        std::string text = " ";
        if (!t->icon().empty()) text += t->icon() + " ";
        text += "[" + std::to_string(i + 1) + "] ";
        if (!t->title().empty()) text += t->title();
        text += " ";
        // Count UTF-8 codepoints
        int w = 0;
        const char* p = text.c_str();
        const char* end = p + text.size();
        while (p < end) {
            int sl = utf8::seqLen(static_cast<uint8_t>(*p));
            w++;
            p += std::min(sl, static_cast<int>(end - p));
        }
        w += 1; // separator
        if (clickCol >= col && clickCol < col + w)
            return i;
        col += w;
    }
    return -1;
}


void PlatformDawn::onMouseButton(int button, int action, int mods)
{
    Tab* tab = activeTab();
    if (!tab) return;

    lastMods_ = glfwModsToModifiers(mods);

    double x, y;
    glfwGetCursorPos(glfwWindow_, &x, &y);
    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    // Clear selection drag on release and finalize selection
    if (action == GLFW_RELEASE && selectionDragActive_) {
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
            ev.button = NoButton;
            ev.modifiers = lastMods_;
            term2->mouseReleaseEvent(&ev);
#if defined(__linux__)
            // Publish the completed selection as the X11 primary selection
            if (term2->hasSelection()) {
                std::string sel = term2->selectedText();
                if (!sel.empty())
                    glfwSetX11SelectionString(sel.c_str());
            }
#endif
        }
        return;
    }

    // 1. Hit test — determine region
    MouseRegion region = hitTest(sx, sy);

    // 2. Click on inactive pane — switch focus (side effect)
    if (action == GLFW_PRESS && region == MouseRegion::Pane && !tab->hasOverlay()) {
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
                    std::string type = (action == GLFW_PRESS) ? "press" : "release";
                    int btn = (button == GLFW_MOUSE_BUTTON_LEFT) ? 0
                            : (button == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 2;
                    scriptEngine_.deliverPopupMouseEvent(
                        clickPane->id(), popup.id, type,
                        relCol, relRow,
                        static_cast<int>(sx), static_cast<int>(sy), btn);
                    scriptEngine_.executePendingJobs();
                    return;
                }
            }

            // Deliver pane mouse event to JS (non-consuming — normal flow continues)
            std::string paneEvtType = (action == GLFW_PRESS) ? "press" : "release";
            int paneBtn = (button == GLFW_MOUSE_BUTTON_LEFT) ? 0
                        : (button == GLFW_MOUSE_BUTTON_RIGHT) ? 1 : 2;
            scriptEngine_.deliverPaneMouseEvent(
                clickPane->id(), paneEvtType,
                cellCol, cellRow,
                static_cast<int>(sx), static_cast<int>(sy), paneBtn);
        }
    }

    // 3. Convert GLFW button to MouseButton
    MouseButton mb = glfwButtonToMouseButton(button);

    // 4. Feed to click detector
    ClickDetector::Result clickResult;
    if (action == GLFW_PRESS) {
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
            ? resolveTabBarClickIndex(sx, sy, tabBarCharWidth_, fbWidth_, fbHeight_, tab, tabs_)
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
                break;
            }
            case Action::SelectionType::Word:
                term->startWordSelection(col, absRow);
#if defined(__linux__)
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) glfwSetX11SelectionString(s.c_str()); }
#endif
                break;
            case Action::SelectionType::Line:
                term->startLineSelection(absRow);
#if defined(__linux__)
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) glfwSetX11SelectionString(s.c_str()); }
#endif
                break;
            case Action::SelectionType::Extend:
                term->extendSelection(col, absRow);
#if defined(__linux__)
                if (term->hasSelection()) { auto s = term->selectedText(); if (!s.empty()) glfwSetX11SelectionString(s.c_str()); }
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
            if (t) {
#if defined(__linux__)
                const char* sel = glfwGetX11SelectionString();
                if (sel && sel[0])
                    t->pasteText(std::string(sel));
#else
                const char* clip = glfwGetClipboardString(glfwWindow_);
                if (clip && clip[0])
                    t->pasteText(std::string(clip));
#endif
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
        ev.modifiers = lastMods_;
        switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:   ev.button = LeftButton; break;
        case GLFW_MOUSE_BUTTON_MIDDLE: ev.button = MidButton;  break;
        case GLFW_MOUSE_BUTTON_RIGHT:  ev.button = RightButton; break;
        default: ev.button = NoButton; break;
        }
        ev.buttons = ev.button;

        if (action == GLFW_PRESS) term->mousePressEvent(&ev);
        else term->mouseReleaseEvent(&ev);
    }
}


void PlatformDawn::onCursorPos(double x, double y)
{
    Tab* tab = activeTab();
    if (!tab) return;
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? static_cast<TerminalEmulator*>(fp->terminal()) : nullptr);
    if (!term) return;

    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

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
        ev.button = NoButton;
        ev.modifiers = lastMods_;
        if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
            ev.buttons |= LeftButton;
        if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
            ev.buttons |= MidButton;
        if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            ev.buttons |= RightButton;
        return ev;
    };

    // 1. If selection drag is active, forward directly to terminal
    if (selectionDragActive_) {
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
    bool buttonHeld = (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
                   || (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
                   || (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

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
    uv_timer_start(&autoScrollTimer_, [](uv_timer_t* t) {
        static_cast<PlatformDawn*>(t->data)->doAutoScroll();
    }, 0, 50);
    autoScrollTimerActive_ = true;
}

void PlatformDawn::stopAutoScroll()
{
    if (!autoScrollTimerActive_) return;
    uv_timer_stop(&autoScrollTimer_);
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

    needsRedraw_ = true;
}

