#include "PlatformDawn.h"
#include "Log.h"
#include "Utils.h"
#include <sys/ioctl.h>

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

    Tab* tab = activeTab();
    if (!tab) return;

    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        // resizeToRect already called terminal->resize() which sets
        // mResizePending if dimensions changed. TIOCSWINSZ is sent
        // later via flushPendingResize() in the render loop.

        int cols = term->width();
        int rows = term->height();
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;

        auto& rs = paneRenderStates_[pane->id()];
        rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
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
    refreshDividers(tab);
    needsRedraw_ = true;

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

    // Check if click is in tab bar
    if (action == GLFW_PRESS && tabBarVisible()) {
        PaneRect tbRect = tab->layout()->tabBarRect(fbWidth_, fbHeight_);
        if (!tbRect.isEmpty() &&
            sx >= tbRect.x && sx < tbRect.x + tbRect.w &&
            sy >= tbRect.y && sy < tbRect.y + tbRect.h) {
            // Determine which tab was clicked by accumulated widths
            if (tabBarCharWidth_ > 0.0f) {
                int clickCol = static_cast<int>((sx - tbRect.x) / tabBarCharWidth_);
                int col = 0;
                for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
                    Tab* t = tabs_[i].get();
                    // Compute tab display width: same as renderTabBar logic
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
                        uint8_t b = static_cast<uint8_t>(*p);
                        int seqLen = (b < 0x80) ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
                        w++;
                        p += std::min(seqLen, static_cast<int>(end - p));
                    }
                    w += 1; // separator
                    if (clickCol >= col && clickCol < col + w) {
                        if (button == GLFW_MOUSE_BUTTON_LEFT) {
                            clearDividers(activeTab());
                            releaseTabTextures(activeTab());
                            activeTabIdx_ = i;
                            refreshDividers(activeTab());
                            updateWindowTitle();
                            tabBarDirty_ = true;
                            needsRedraw_ = true;
                        } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
                            closeTab(i);
                        }
                        return;
                    }
                    col += w;
                }
            }
            return;
        }
    }

    // Click on an inactive pane — switch focus before routing the event
    if (action == GLFW_PRESS && !tab->hasOverlay()) {
        int clickedId = tab->layout()->paneAtPixel(static_cast<int>(sx), static_cast<int>(sy));
        if (clickedId >= 0 && clickedId != tab->layout()->focusedPaneId()) {
            int prev = tab->layout()->focusedPaneId();
            tab->layout()->setFocusedPane(clickedId);
            notifyPaneFocusChange(tab, prev, clickedId);
            updateTabTitleFromFocusedPane(activeTabIdx_);
        }
    }

    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? fp->activeTerm() : nullptr);
    if (!term) return;

    // Adjust for pane origin
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
    case GLFW_MOUSE_BUTTON_LEFT:  ev.button = LeftButton; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: ev.button = MidButton; break;
    case GLFW_MOUSE_BUTTON_RIGHT: ev.button = RightButton; break;
    default: ev.button = NoButton; break;
    }
    ev.buttons = ev.button;

    // Cmd/Ctrl+click on hyperlinks opens the URL
    if (action == GLFW_RELEASE && ev.button == LeftButton &&
        (ev.modifiers & (MetaModifier | CtrlModifier))) {
        int col = ev.x, row = ev.y;
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
    }

    if (action == GLFW_PRESS) term->mousePressEvent(&ev);
    else term->mouseReleaseEvent(&ev);
}


void PlatformDawn::onCursorPos(double x, double y)
{
    Tab* tab = activeTab();
    if (!tab) return;
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? fp->activeTerm() : nullptr);
    if (!term) return;

    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
    double relX = sx - pr.x;
    double relY = sy - pr.y;

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
    term->mouseMoveEvent(&ev);
}

