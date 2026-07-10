#include "InputController.h"

#include "AnimationScheduler.h"
#include "DividerResizeDrag.h"
#include "LayoutTree.h"
#include "PlatformDawn.h"
#include "RenderThread.h"
#include "ScriptEngine.h"
#include "SelectionDrag.h"
#include "TabReorderDrag.h"
#include "Terminal.h"
#include "Utf8.h"

#include <spdlog/spdlog.h>

// Declared in PlatformDawn.h; implemented per-platform.
void platformOpenURL(const std::string &url);

static std::string codepointToUtf8(uint32_t cp)
{
    return utf8::encode(cp);
}

// Wezterm's ctrl_mapping table (wezterm-input-types/src/lib.rs:2235).
// Returns the legacy control byte for Ctrl+<ch>, or -1 if not in the
// table (in which case the caller should emit `ch` literally per
// wezterm's csi_u_encode final fallback). Aliases match wezterm:
// `@` / `` ` `` / space / `2` → NUL, etc. Both Shift'd and unshifted
// forms of bracket/symbol keys are accepted so the caller doesn't have
// to resolve Shift before lookup.
static int ctrlMappingForChar(uint32_t ch)
{
    switch (ch) {
        case ' ':
        case '@':
        case '`':
        case '2': return 0x00;
        case '[':
        case '{':
        case '3': return 0x1b;
        case '\\':
        case '|':
        case '4': return 0x1c;
        case ']':
        case '}':
        case '5': return 0x1d;
        case '^':
        case '~':
        case '6': return 0x1e;
        case '_':
        case '/':
        case '7': return 0x1f;
        case '?':
        case '8': return 0x7f;
        default: return -1;
    }
}

static MouseButton buttonToMouseButton(int button)
{
    switch (button) {
        case static_cast<int>(LeftButton): return MouseButton::Left;
        case static_cast<int>(MidButton): return MouseButton::Middle;
        case static_cast<int>(RightButton): return MouseButton::Right;
        default: return MouseButton::Left;
    }
}

InputController::InputController()  = default;
InputController::~InputController() = default;

Window::CursorStyle InputController::pointerShapeNameToCursorStyle(const std::string &name)
{
    using CS = Window::CursorStyle;
    if (name.empty() || name == "default" || name == "left_ptr" ||
        name == "context-menu" || name == "alias" || name == "copy" ||
        name == "dnd-link" || name == "dnd-copy" || name == "dnd-none" ||
        name == "none") {
        return CS::Arrow;
    }
    if (name == "text" || name == "vertical-text" ||
        name == "xterm" || name == "ibeam") {
        return CS::IBeam;
    }
    if (name == "pointer" || name == "pointing_hand" || name == "hand" ||
        name == "hand1" || name == "hand2" || name == "openhand" ||
        name == "closedhand" || name == "grab" || name == "grabbing") {
        return CS::Pointer;
    }
    if (name == "crosshair" || name == "tcross" || name == "cross" ||
        name == "cell" || name == "plus") {
        return CS::Crosshair;
    }
    if (name == "wait" || name == "clock" || name == "watch" ||
        name == "progress" || name == "half-busy" ||
        name == "left_ptr_watch") {
        return CS::Wait;
    }
    if (name == "help" || name == "question_arrow" ||
        name == "whats_this") {
        return CS::Help;
    }
    if (name == "move" || name == "fleur" || name == "all-scroll" ||
        name == "pointer-move") {
        return CS::Move;
    }
    if (name == "not-allowed" || name == "no-drop" ||
        name == "forbidden" || name == "crossed_circle" ||
        name == "dnd-no-drop") {
        return CS::NotAllowed;
    }
    if (name == "ew-resize" || name == "e-resize" || name == "w-resize" ||
        name == "col-resize" || name == "right_side" ||
        name == "left_side" || name == "sb_h_double_arrow" ||
        name == "split_h") {
        return CS::ResizeH;
    }
    if (name == "ns-resize" || name == "n-resize" || name == "s-resize" ||
        name == "row-resize" || name == "top_side" ||
        name == "bottom_side" || name == "sb_v_double_arrow" ||
        name == "split_v") {
        return CS::ResizeV;
    }
    if (name == "nesw-resize" || name == "ne-resize" || name == "sw-resize" ||
        name == "top_right_corner" || name == "bottom_left_corner" ||
        name == "size_bdiag" || name == "size-bdiag") {
        return CS::ResizeNESW;
    }
    if (name == "nwse-resize" || name == "nw-resize" || name == "se-resize" ||
        name == "top_left_corner" || name == "bottom_right_corner" ||
        name == "size_fdiag" || name == "size-fdiag") {
        return CS::ResizeNWSE;
    }
    if (name == "zoom-in" || name == "zoom_in" ||
        name == "zoom-out" || name == "zoom_out") {
        return CS::Crosshair;
    }
    return CS::Arrow;
}

// key, scancode, action, mods are already platform-independent (Key enum, KeyAction enum, Modifier bitmask)
// — the Window backend (XCBWindow/CocoaWindow) does all conversion before calling here.
void InputController::onKey(int key, int scancode, int action, int mods)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());

    TerminalEmulator *term = static_cast<TerminalEmulator *>(platform_->activeTerm());
    if (!term) {
        return;
    }

    Window *window = platform_->window_.get();

    spdlog::debug("onKey: key=0x{:x} action={} mods={}", key, action, mods);

    controlPressed_         = (mods & CtrlModifier) != 0;
    const uint32_t prevMods = lastMods_;
    lastMods_               = static_cast<uint32_t>(mods);
    if (lastMods_ != prevMods) {
        // Modifier transition (e.g. Ctrl pressed/released) may flip whether a
        // Left+Click on the cell under the cursor would open a hyperlink, so
        // refresh the pointer shape without waiting for mouse motion.
        refreshPointerShape();
    }

    Key k = static_cast<Key>(key);
    spdlog::debug("onKey: key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // Tab-drag cancel. Escape during an in-progress reorder rewinds via a
    // single moveChild back to the starting index, drops the highlight, and
    // consumes the key so it doesn't also dispatch a keybinding / Esc-defocus /
    // selected-command-clear below. Press/repeat only; release is irrelevant.
    if (action != static_cast<int>(KeyAction_Release) && k == Key_Escape) {
        if (auto *td = dynamic_cast<TabReorderDrag *>(activeDrag_.get())) {
            if (td->started()) {
                int delta = td->dragStartIndex() - td->currentIndex();
                if (delta != 0) {
                    platform_->scriptEngine_.layoutTree().moveChild(td->stackId(), td->draggedTabId(), delta);
                    platform_->tabBarDirty_ = true;
                    platform_->setNeedsRedraw();
                }
            }
            platform_->clearDraggedTab();
            activeDrag_.reset();
            return;
        }
    }

    // Bindings only on press/repeat (NOT release). Mask off
    // OptionAsAltModifier — it's an internal signal for the ESC-prefix
    // path, not a user-facing modifier in keybinding strings.
    if (action != static_cast<int>(KeyAction_Release)) {
        auto result = sequenceMatcher_.advance({ k, lastMods_ & kBindingModMask }, bindings_);
        if (result.result == SequenceMatcher::Result::Match) {
            pendingSequenceKeys_.clear();
            cancelSequenceTimeout();
            for (const auto &act : result.actions) {
                platform_->dispatchAction(act);
            }
            return;
        }
        if (result.result == SequenceMatcher::Result::Prefix) {
            pendingSequenceKeys_.push_back({ key, scancode, action, mods });
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
            for (const auto &p : pending) {
                replayPendingSequenceKey(p);
            }
        }
    }

    // OSC 133 highlight: Escape (no modifiers) clears the selected command
    // region and is swallowed. Only takes effect while a command is selected,
    // so normal Escape delivery to TUI apps is preserved at other times.
    // selectedCommandId() reads parse-mutated state — only acquire the
    // per-Terminal lock when the cheap key/mods test already matches.
    // Otherwise every keystroke pays the lock-wait of the parser, which
    // can be hundreds of ms during a flood.
    if (action != static_cast<int>(KeyAction_Release) &&
        k == Key_Escape && mods == 0) {
        bool hasSelectedCmd;
        {
            std::lock_guard<std::recursive_mutex> _lk(term->mutex());
            hasSelectedCmd = term->selectedCommandId().has_value();
        }
        if (hasSelectedCmd) {
            term->setSelectedCommand(std::nullopt);
            return;
        }
    }

    // Esc defocus for embedded terminals. Applets use `q` / Ctrl-C for their
    // own cancel, so Esc is repurposed here as "hand focus back to the
    // parent pane". The focused popup path intentionally forwards Esc to the
    // popup's content; embeddeds differ.
    if (action != static_cast<int>(KeyAction_Release) &&
        k == Key_Escape && mods == 0) {
        auto tab = platform_->activeTab();
        if (tab) {
            Terminal *fp = platform_->scriptEngine_.focusedTerminalInSubtree(*tab);
            if (fp && fp->focusedEmbeddedLineId() != 0) {
                fp->clearFocusedEmbedded();
                fp->focusEvent(true);
                platform_->renderThread_->pending().dirtyPanes.insert(fp->id());
                platform_->setNeedsRedraw();
                return;
            }
        }
    }

    // Reset viewport to live + restart the cursor blink when typing.
    // Skipped on:
    //   - releases — only press/repeat counts as "typing".
    //   - bindings that matched — they returned earlier, never reach here.
    //   - bare modifier presses (Ctrl alone, Shift alone, ...) — the
    //     user is mid-gesture (Ctrl-click on a URL in the scrollback,
    //     about to start a Cmd+T binding, etc.) and jumping the
    //     viewport / restarting the blink mid-gesture is jarring.
    //     The same isModifierKey predicate gates the emulator-side
    //     selection clear and viewport reset in keyPressEvent.
    if (action != static_cast<int>(KeyAction_Release) && !isModifierKey(k)) {
        term->resetViewport();
        if (platform_->animScheduler_) {
            platform_->animScheduler_->resetBlink();
        }
    }

    KeyEvent ev;
    ev.key        = k;
    ev.modifiers  = lastMods_;
    ev.action     = static_cast<KeyAction>(action);
    ev.autoRepeat = (action == static_cast<int>(KeyAction_Repeat));
    ev.count      = 1;

    if (term->kittyFlags() != 0) {
        // Kitty mode: text is the character the key produces; modifiers are
        // encoded separately in the CSI u sequence. For ctrl+letter, send the
        // lowercase letter, not the control character.
        if (controlPressed_ && ((key >= Key_A && key <= Key_Z) || (key >= 0x61 && key <= 0x7a))) {
            char ch = (key >= 0x61) ? static_cast<char>(key) : static_cast<char>(key - Key_A + 'a');
            ev.text = std::string(1, ch);
        } else if (key >= Key_Space && key <= Key_AsciiTilde) {
#if defined(__APPLE__)
            // macOS delivers onChar (the OS-composed text) *before* onKey for
            // the same keystroke, so prefer that composed character — it
            // reflects Shift / CapsLock / AltGr / dead-key composition, which
            // keyName (layout level-0, modifier-independent on macOS) does not.
            // Matched by scancode and consumed so it can't bleed into a later
            // key; falls back to keyName when nothing matched.
            if (scancode >= 0 && lastComposed_.scancode == scancode) {
                ev.text                = codepointToUtf8(lastComposed_.codepoint);
                ev.unshiftedKey        = lastComposed_.unshifted;
                lastComposed_.scancode = -1; // consume
            } else {
                std::string name = window ? window->keyName(scancode) : std::string {};
                if (!name.empty()) {
                    ev.text = name;
                }
            }
#else
            // X11/Wayland: keyName already reflects the active modifiers via the
            // live xkb state, and onChar arrives *after* onKey — so the stash
            // would be a stale previous keystroke (it matches on a repeated
            // physical key). Use keyName directly; it is current-state-correct.
            std::string name = window ? window->keyName(scancode) : std::string {};
            if (!name.empty()) {
                ev.text = name;
            }
#endif
        }
        // Populate shifted_key + base_layout_key for report_alternate_key mode
        if (window) {
            ev.shiftedKey    = window->shiftedKeyCodepoint(scancode);
            ev.baseLayoutKey = window->baseLayoutKeyCodepoint(scancode);
        }
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy mode: drop release events
    if (action == static_cast<int>(KeyAction_Release)) {
        return;
    }

    // ctrl+letter: generate control character (handle both upper and lower keysyms)
    if (controlPressed_ && ((key >= Key_A && key <= Key_Z) || (key >= 0x61 && key <= 0x7a))) {
        int offset = (key >= 0x61) ? (key - 0x61) : (key - Key_A);
        ev.text    = std::string(1, static_cast<char>(offset + 1));
        spdlog::debug("onKey: ctrl+letter, sending text=0x{:02x}", static_cast<unsigned char>(ev.text[0]));
        term->keyPressEvent(&ev);
        return;
    }

    // ctrl + space / punctuation / digit → control byte (xterm legacy + the
    // wezterm `ctrl_mapping` table — wezterm-input-types/src/lib.rs:2235).
    // The byte depends on the *resulting* character the user typed, not the
    // base keysym, so e.g. Ctrl+Shift+2 on US (which produces '@') must map
    // to 0x00 even though the base keysym is Key_2. Use the shifted codepoint
    // when Shift is held, the base keyName otherwise.
    //
    //   ctrl+space  ctrl+@  ctrl+`  ctrl+2  → 0x00 (NUL)
    //   ctrl+[      ctrl+{  ctrl+3          → 0x1b (ESC)  [same as bare Esc]
    //   ctrl+\      ctrl+|  ctrl+4          → 0x1c (FS)
    //   ctrl+]      ctrl+}  ctrl+5          → 0x1d (GS)
    //   ctrl+^      ctrl+~  ctrl+6          → 0x1e (RS)
    //   ctrl+_      ctrl+/  ctrl+7          → 0x1f (US)
    //   ctrl+?      ctrl+8                  → 0x7f (DEL)
    //
    // Ctrl + (0,1,9, or any punctuation not above) → just the char itself
    // (matching wezterm's csi_u_encode final fallback). If Alt is also held,
    // prepend ESC. Done here rather than in onChar because onChar drops
    // everything when controlPressed_ is set.
    if (controlPressed_ && window && key < 0x01000000 && k != Key_unknown) {
        uint32_t ch = 0;
        if ((lastMods_ & ShiftModifier) != 0) {
            ch = window->shiftedKeyCodepoint(scancode);
        }
        if (ch == 0) {
            std::string base = window->keyName(scancode);
            if (!base.empty()) {
                ch = static_cast<unsigned char>(base[0]);
            }
        }
        int mapped = ctrlMappingForChar(ch);
        if (mapped >= 0) {
            char outByte         = static_cast<char>(mapped);
            const bool altPrefix = (lastMods_ & OptionAsAltModifier) != 0;
            ev.text              = altPrefix ? std::string("\x1b") + outByte : std::string(1, outByte);
            spdlog::debug("onKey: ctrl+punct ch=0x{:02x} → 0x{:02x}{}",
                          ch,
                          static_cast<unsigned char>(outByte),
                          altPrefix ? " (alt-prefixed)" : "");
            term->keyPressEvent(&ev);
            return;
        }
    }

    // Alt+printable → ESC prefix (xterm convention, required for readline
    // word-nav like M-b / M-f). We have to handle it here rather than in
    // onChar because on macOS the OS text-input system composes option+letter
    // into a different character (e.g. Alt+B → "∫") by the time onChar fires.
    // Use the window's keyName(scancode), which returns the layout-correct
    // base character regardless of current modifier state, so Dvorak etc.
    // stay correct. The duplicate onChar (from the OS text path on Linux, or
    // from a non-skipped interpretKeyEvents on macOS) is dropped by the
    // OptionAsAltModifier guard in onChar below. Gating on OptionAsAltModifier
    // (set by the platform layer when this Option side is configured as Alt)
    // rather than AltModifier means the right-Option-as-compose case still
    // delivers macOS-composed characters via onChar untouched.
    if (altSendsEsc_ && (lastMods_ & OptionAsAltModifier) && !(lastMods_ & CtrlModifier) &&
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

void InputController::onChar(uint32_t codepoint, uint32_t unshiftedCodepoint, int scancode)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    // Record the OS-composed character so a matching onKey (kitty mode) can use
    // it as the text/keyCode instead of the layout level-0 keyName. Stashed
    // before the kitty early-return below so it's available even though kitty
    // mode otherwise drops the onChar text path.
    lastComposed_ = { scancode, codepoint, unshiftedCodepoint };

    TerminalEmulator *term = static_cast<TerminalEmulator *>(platform_->activeTerm());
    if (!term) {
        return;
    }

    spdlog::debug("onChar: codepoint=U+{:04X} unshifted=U+{:04X} controlPressed={}", codepoint, unshiftedCodepoint, controlPressed_);
    term->resetViewport();
    if (platform_->animScheduler_) {
        platform_->animScheduler_->resetBlink();
    }

    // In Kitty mode, onKey handles everything
    if (term->kittyFlags() != 0) {
        return;
    }

    if (controlPressed_) {
        return;
    }

    // When alt_sends_esc is on and this Option side is configured as Alt,
    // onKey has already emitted ESC+<base-char>. The OS text path still
    // fires onChar (Linux always; macOS only when skip-IME in Window_cocoa
    // didn't catch it). Drop it so the shell doesn't see the character
    // twice. Crucially: if the side is NOT configured as Alt, we want the
    // composed character to flow through (e.g. right-Option=compose for
    // typing accents), so the gate must check OptionAsAltModifier — not
    // bare AltModifier.
    if (altSendsEsc_ && (lastMods_ & OptionAsAltModifier) && !(lastMods_ & CtrlModifier)) {
        return;
    }

    KeyEvent ev;
    ev.key          = Key_unknown;
    ev.text         = codepointToUtf8(codepoint);
    ev.unshiftedKey = unshiftedCodepoint;
    // shiftedKey is the codepoint with shift applied. The OS-composed
    // codepoint IS the shifted variant when shift is held and it differs
    // from the unshifted form. Populate so REPORT_ALTERNATE_KEYS emits
    // the `keycode:shifted_key` form for non-functional keys.
    if ((lastMods_ & ShiftModifier) && unshiftedCodepoint != 0 && codepoint != unshiftedCodepoint) {
        ev.shiftedKey = codepoint;
    }
    ev.modifiers = lastMods_;
    ev.action    = KeyAction_Press;
    spdlog::debug("onChar: sending text='{}' ({} bytes) unshifted=U+{:04X}", ev.text, ev.text.size(), unshiftedCodepoint);
    ev.count      = 1;
    ev.autoRepeat = false;
    term->keyPressEvent(&ev);
}

MouseRegion InputController::hitTest(double sx, double sy)
{
    auto tab = platform_->activeTab();
    if (!tab) {
        return MouseRegion::Pane;
    }
    Script::Engine &eng = platform_->scriptEngine_;

    // Check every visible TabBar. `tabBarRects` covers the root primary
    // bar plus any sub-bars inside the currently-active top-level tab.
    // Tests every rect (in tree-walk order), returns on the first hit.
    for (const auto &[barId, tbRect] : eng.tabBarRects(platform_->fbWidth_, platform_->fbHeight_)) {
        if (sx >= tbRect.x && sx < tbRect.x + tbRect.w &&
            sy >= tbRect.y && sy < tbRect.y + tbRect.h) {
            (void)barId;
            return MouseRegion::TabBar;
        }
    }

    // Divider hit-test — suppresses pane-selection and terminal forwarding when
    // the click lands on a split divider rather than inside a pane.
    // When zoomed, dividerRects returns nothing: the tree's rect map skips
    // non-zoomed siblings, so no dividers emit from dividersIn. Safe to drop
    // the explicit !isZoomed guard.
    //
    // `divider_hit_pad` from config expands the hit rect on every side so a
    // 1-px-wide divider is easy to grab for resize. Visual rect is unchanged.
    const int divPx = eng.dividerPixels();
    if (divPx > 0) {
        const int pad = std::max(0, platform_->lastConfig().divider_hit_pad);
        for (const auto &r : eng.tabDividerRects(*tab, divPx)) {
            if (sx >= r.x - pad && sx < r.x + r.w + pad &&
                sy >= r.y - pad && sy < r.y + r.h + pad) {
                return MouseRegion::Divider;
            }
        }
    }
    return MouseRegion::Pane;
}

// Resolve which TabBar was clicked and which tab index within it. Reads
// the per-bar render state under the render-thread mutex (caller holds
// it). With nested TabBars (wrapInStack-created sub-bars), the per-bar
// `colRanges` are the source of truth for click-to-index mapping;
// PlatformDawn no longer maintains a singular ranges vector.
int InputController::resolveTabBarClickIndex(double sx, double sy, Uuid *outBarId)
{
    if (outBarId) {
        *outBarId = {};
    }
    float tbCharWidth = platform_->tabBarCharWidth_;
    if (tbCharWidth <= 0.0f) {
        return -1;
    }
    if (!platform_->activeTab()) {
        return -1;
    }

    const auto &bars = platform_->renderThread_->renderState().tabBars;
    for (const TabBarRender &bar : bars) {
        if (bar.rect.isEmpty()) {
            continue;
        }
        if (sx < bar.rect.x || sx >= bar.rect.x + bar.rect.w ||
            sy < bar.rect.y || sy >= bar.rect.y + bar.rect.h) {
            continue;
        }
        const int clickCol = static_cast<int>((sx - bar.rect.x) / tbCharWidth);
        for (int i = 0; i < static_cast<int>(bar.colRanges.size()); ++i) {
            auto [start, end] = bar.colRanges[i];
            if (start < 0) {
                continue; // tab not visible within this bar (overflow-trimmed)
            }
            if (clickCol >= start && clickCol < end) {
                if (outBarId) {
                    *outBarId = bar.id;
                }
                return i;
            }
        }
        // Hit fell in a non-tab column of this bar (overflow indicator etc.).
        if (outBarId) {
            *outBarId = bar.id;
        }
        return -1;
    }
    return -1;
}

void InputController::onMouseButton(int button, int action, int mods)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    auto tab = platform_->activeTab();
    if (!tab) {
        return;
    }
    Script::Engine &eng = platform_->scriptEngine_;

    Window *window = platform_->window_.get();

    lastMods_ = static_cast<uint32_t>(mods);

    // Track button state for use in onCursorPos (replaces glfwGetMouseButton polling)
    if (action == static_cast<int>(KeyAction_Press)) {
        heldButtons_ |= static_cast<uint32_t>(button);
    } else {
        heldButtons_ &= ~static_cast<uint32_t>(button);
    }

    const float contentScaleX = platform_->contentScaleX_;
    const float contentScaleY = platform_->contentScaleY_;
    const float charWidth     = platform_->charWidth_;
    const float lineHeight    = platform_->lineHeight_;
    const float padLeft       = platform_->padLeft_;
    const float padTop        = platform_->padTop_;
    const uint32_t fbWidth    = platform_->fbWidth_;
    const uint32_t fbHeight   = platform_->fbHeight_;

    double sx = lastCursorX_ * contentScaleX;
    double sy = lastCursorY_ * contentScaleY;

    // Clear divider-resize drag on release. resizeEdgeAlongAxis already
    // applied each pixel delta incrementally, so the final state is in
    // place; just drop the handler.
    if (action == static_cast<int>(KeyAction_Release) && dynamic_cast<DividerResizeDrag *>(activeDrag_.get())) {
        activeDrag_.reset();
        return;
    }

    // Clear tab-drag on release. Moves already happened mid-drag via
    // LayoutTree::moveChild, so the release path just tears down the
    // highlight and drops the handler. Returns early — the rest of this
    // method assumes a Terminal-pane mouse interaction.
    if (action == static_cast<int>(KeyAction_Release) && dynamic_cast<TabReorderDrag *>(activeDrag_.get())) {
        platform_->clearDraggedTab();
        activeDrag_.reset();
        return;
    }

    // Clear selection drag on release and finalize selection
    if (action == static_cast<int>(KeyAction_Release) && dynamic_cast<SelectionDrag *>(activeDrag_.get())) {
        activeDrag_.reset();
        stopAutoScroll();
        Terminal *fp2           = eng.focusedTerminalInSubtree(*tab);
        TerminalEmulator *term2 = static_cast<TerminalEmulator *>(fp2);
        if (term2) {
            Rect pr         = fp2 ? fp2->rect() : Rect { 0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight) };
            double cellRelX = sx - pr.x - padLeft;
            double cellRelY = sy - pr.y - padTop;
            MouseEvent ev;
            ev.x          = static_cast<int>(cellRelX / charWidth);
            ev.y          = static_cast<int>(cellRelY / lineHeight);
            ev.globalX    = static_cast<int>(sx);
            ev.globalY    = static_cast<int>(sy);
            ev.pixelX     = static_cast<int>(cellRelX);
            ev.pixelY     = static_cast<int>(cellRelY);
            ev.xRightHalf = (cellRelX - ev.x * charWidth) >= (charWidth * 0.5);
            ev.button     = NoButton;
            ev.modifiers  = lastMods_;
            term2->mouseReleaseEvent(&ev);
#if defined(__linux__)
            // Publish the completed selection as the X11 primary selection.
            // hasSelection / selectedText read parse-mutated state.
            std::string sel;
            {
                std::lock_guard<std::recursive_mutex> _lk(term2->mutex());
                if (term2->hasSelection()) {
                    sel = term2->selectedText();
                }
            }
            if (!sel.empty() && window) {
                window->setPrimarySelection(sel);
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
    if (region == MouseRegion::Divider) {
        // Left-press on a divider installs a DividerResizeDrag. The motion
        // path in onCursorPos drives resizeTabPaneEdge with per-event pixel
        // deltas along the divider's axis. Convert `button` via
        // buttonToMouseButton — the int parameter uses the bitmask `Button`
        // enum (LeftButton=0x1) while MouseButton is a separate enum class
        // with Left=0; a raw static_cast would silently misclassify.
        if (action == static_cast<int>(KeyAction_Press) && buttonToMouseButton(button) == MouseButton::Left) {
            const int divPx = eng.dividerPixels();
            if (divPx > 0) {
                const int pad = std::max(0, platform_->lastConfig().divider_hit_pad);
                Uuid ownerPaneId;
                SplitDir axis = SplitDir::Horizontal;
                bool found    = false;
                for (const auto &[paneId, r] : eng.tabDividersWithOwnerPanes(*tab, divPx)) {
                    if (sx >= r.x - pad && sx < r.x + r.w + pad && sy >= r.y - pad && sy < r.y + r.h + pad) {
                        ownerPaneId = paneId;
                        // Vertical-strip divider (w < h) sits between
                        // left/right siblings — parent split is Horizontal.
                        // Horizontal-strip divider (h < w) sits between
                        // top/bottom siblings — parent split is Vertical.
                        axis        = (r.w < r.h) ? SplitDir::Horizontal : SplitDir::Vertical;
                        found       = true;
                        break;
                    }
                }
                if (found) {
                    activeDrag_ = std::make_unique<DividerResizeDrag>(
                        sx,
                        sy,
                        buttonToMouseButton(button),
                        ownerPaneId,
                        *tab,
                        axis);
                    if (window) {
                        window->setCursorStyle(activeDrag_->cursorStyle());
                    }
                }
            }
        }
        return;
    }

    // 2. Click on inactive pane — switch focus (side effect)
    if (action == static_cast<int>(KeyAction_Press) && region == MouseRegion::Pane) {
        Uuid clickedId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
        Uuid curFocus  = eng.focusedPaneInSubtree(*tab);
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
    if (region == MouseRegion::Pane) {
        Terminal *clickPane = eng.focusedTerminalInSubtree(*tab);
        if (clickPane) {
            const Rect &pr  = clickPane->rect();
            double cellRelX = sx - pr.x - padLeft;
            double cellRelY = sy - pr.y - padTop;
            int cellCol     = static_cast<int>(cellRelX / charWidth);
            int cellRow     = static_cast<int>(cellRelY / lineHeight);
            for (const auto &popup : clickPane->popups()) {
                if (cellCol >= popup->cellX() && cellCol < popup->cellX() + popup->cellW() &&
                    cellRow >= popup->cellY() && cellRow < popup->cellY() + popup->cellH()) {
                    int relCol              = cellCol - popup->cellX();
                    int relRow              = cellRow - popup->cellY();
                    int relPixelX           = static_cast<int>(cellRelX) - popup->cellX() * static_cast<int>(charWidth);
                    int relPixelY           = static_cast<int>(cellRelY) - popup->cellY() * static_cast<int>(lineHeight);
                    std::string popupIdCopy = popup->popupId();

                    // Deliver mouse event to JS popup listeners. The JS handler
                    // may call popup.close() synchronously, which extracts the
                    // popup from clickPane->popups() and invalidates the
                    // reference we're iterating. Don't touch `popup` after
                    // this point — re-resolve by id.
                    std::string type = (action == static_cast<int>(KeyAction_Press)) ? "press" : "release";
                    int btn          = (button == static_cast<int>(LeftButton)) ? 0
                                 : (button == static_cast<int>(RightButton))    ? 1
                                                                                : 2;
                    platform_->scriptEngine_.deliverPopupMouseEvent(
                        clickPane->id(),
                        popupIdCopy,
                        type,
                        relCol,
                        relRow,
                        static_cast<int>(sx),
                        static_cast<int>(sy),
                        btn);
                    platform_->scriptEngine_.executePendingJobs();

                    // VT mouse-reporting forward. If the applet has
                    // DECSET 1000/1002/1006 active inside the popup, its
                    // emulator serializes the click to SGR bytes via
                    // writeToOutput -> onInput -> JS "input" listener.
                    // Skipped when reporting is inactive so we don't arm
                    // a spurious text selection on a headless popup.
                    Terminal *livePopup = nullptr;
                    for (const auto &p : clickPane->popups()) {
                        if (p->popupId() == popupIdCopy) {
                            livePopup = p.get();
                            break;
                        }
                    }
                    if (livePopup && livePopup->mouseReportingActive()) {
                        MouseEvent mev;
                        mev.x         = relCol;
                        mev.y         = relRow;
                        mev.globalX   = static_cast<int>(sx);
                        mev.globalY   = static_cast<int>(sy);
                        mev.pixelX    = relPixelX;
                        mev.pixelY    = relPixelY;
                        mev.button    = static_cast<Button>(button);
                        mev.modifiers = lastMods_;
                        if (action == static_cast<int>(KeyAction_Press)) {
                            livePopup->mousePressEvent(&mev);
                        } else {
                            livePopup->mouseReleaseEvent(&mev);
                        }
                    }
                    return;
                }
            }

            // 2c. Embedded terminals. Hit-test visual displacement so a click
            // on a visible embedded focuses it. Skipped mid-drag (drag
            // originated outside the embedded — let the selection continue
            // across its rect instead of stealing focus). Also skipped on
            // alt-screen where embeddeds are hidden anyway.
            if (!activeDrag_ && !clickPane->usingAltScreen() &&
                clickPane->hasEmbeddeds()) {
                uint64_t hitLineId = 0;
                int emRelCol = 0, emRelRow = 0, emRelPx = 0, emRelPy = 0;
                bool clickedEmbedded = clickPane->liveSegmentHitTest(
                    cellRelX,
                    cellRelY,
                    static_cast<float>(charWidth),
                    lineHeight,
                    hitLineId,
                    emRelCol,
                    emRelRow,
                    emRelPx,
                    emRelPy);
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
                    int btn          = (button == static_cast<int>(LeftButton)) ? 0
                                 : (button == static_cast<int>(RightButton))    ? 1
                                                                                : 2;
                    platform_->scriptEngine_.deliverEmbeddedMouseEvent(
                        clickPane->id(),
                        hitLineId,
                        type,
                        emRelCol,
                        emRelRow,
                        emRelPx,
                        emRelPy,
                        btn);
                    platform_->scriptEngine_.executePendingJobs();

                    // VT mouse-reporting forward for embedded — same as
                    // the popup path. Only fires when DECSET 1000/1002/
                    // 1006 are active, so the classic no-reporting case
                    // doesn't spuriously arm a text selection.
                    if (Terminal *em = clickPane->findEmbedded(hitLineId)) {
                        if (em->mouseReportingActive()) {
                            MouseEvent mev;
                            mev.x         = emRelCol;
                            mev.y         = emRelRow;
                            mev.globalX   = static_cast<int>(sx);
                            mev.globalY   = static_cast<int>(sy);
                            mev.pixelX    = emRelPx;
                            mev.pixelY    = emRelPy;
                            mev.button    = static_cast<Button>(button);
                            mev.modifiers = lastMods_;
                            if (action == static_cast<int>(KeyAction_Press)) {
                                em->mousePressEvent(&mev);
                            } else {
                                em->mouseReleaseEvent(&mev);
                            }
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
            int paneBtn             = (button == static_cast<int>(LeftButton)) ? 0
                            : (button == static_cast<int>(RightButton))        ? 1
                                                                               : 2;
            platform_->scriptEngine_.deliverPaneMouseEvent(
                clickPane->id(),
                paneEvtType,
                cellCol,
                cellRow,
                static_cast<int>(sx),
                static_cast<int>(sy),
                paneBtn);
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
    Terminal *fp           = eng.focusedTerminalInSubtree(*tab);
    TerminalEmulator *term = static_cast<TerminalEmulator *>(fp);
    if (!term) {
        return;
    }

    // 6. Determine mouse mode
    MouseMode mode = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;

    // 7. Build MouseStroke and match
    MouseStroke stroke;
    stroke.button = mb;
    stroke.mods   = lastMods_ & kBindingModMask;
    stroke.event  = clickResult.type;
    stroke.mode   = mode;
    stroke.region = region;

    auto matched = matchMouseBindings(stroke, mouseBindings_);

    if (!matched.empty()) {
        // Compute cell coordinates relative to pane (subtract padding so col 0
        // begins at the first cell, not the pane edge)
        Rect pr         = fp ? fp->rect() : Rect { 0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight) };
        double cellRelX = sx - pr.x - padLeft;
        double cellRelY = sy - pr.y - padTop;
        int cellCol     = static_cast<int>(cellRelX / charWidth);
        int cellRow     = static_cast<int>(cellRelY / lineHeight);

        mouseCtx_.cellCol          = cellCol;
        mouseCtx_.cellRow          = cellRow;
        mouseCtx_.pixelX           = static_cast<int>(sx);
        mouseCtx_.pixelY           = static_cast<int>(sy);
        mouseCtx_.button           = mb;
        mouseCtx_.tabBarClickBarId = {};
        mouseCtx_.tabBarClickIndex = (region == MouseRegion::TabBar)
            ? resolveTabBarClickIndex(sx, sy, &mouseCtx_.tabBarClickBarId)
            : -1;

        // Tab-bar left-press: install a TabReorderDrag for drag-to-reorder.
        // No state changes here — `started_` only flips on motion past the
        // configured threshold, so a plain click without motion falls through
        // to the existing ActivateTab path below unchanged.
        if (region == MouseRegion::TabBar && mb == MouseButton::Left && !mouseCtx_.tabBarClickBarId.isNil() && mouseCtx_.tabBarClickIndex >= 0) {
            LayoutTree &tree = platform_->scriptEngine_.layoutTree();
            const Node *bar  = tree.node(mouseCtx_.tabBarClickBarId);
            Uuid stackId, draggedTabId;
            if (bar) {
                if (auto *tb = std::get_if<TabBarData>(&bar->data)) {
                    stackId           = tb->boundStack;
                    const Node *stack = tree.node(stackId);
                    if (stack) {
                        if (auto *sd = std::get_if<StackData>(&stack->data)) {
                            if (mouseCtx_.tabBarClickIndex < static_cast<int>(sd->children.size())) {
                                draggedTabId = sd->children[mouseCtx_.tabBarClickIndex].id;
                            }
                        }
                    }
                }
            }
            if (!stackId.isNil() && !draggedTabId.isNil()) {
                int threshold = platform_->lastConfig().tab_bar.drag_threshold_px;
                if (threshold < 0) {
                    threshold = 0;
                }
                // Capture cursor X within the dragged tab so the float
                // follows the cursor with the press point pinned at the
                // same horizontal position. Read the tab's pixel left edge
                // from the render state's colRanges.
                float tabBarCharWidth  = platform_->tabBarCharWidth_;
                float dragOffsetXInTab = 0.0f;
                const auto &bars       = platform_->renderThread_->renderState().tabBars;
                for (const TabBarRender &br : bars) {
                    if (br.id == mouseCtx_.tabBarClickBarId && mouseCtx_.tabBarClickIndex < static_cast<int>(br.colRanges.size())) {
                        float tabLeftPx  = static_cast<float>(br.rect.x) + static_cast<float>(br.colRanges[mouseCtx_.tabBarClickIndex].first) * tabBarCharWidth;
                        dragOffsetXInTab = static_cast<float>(sx) - tabLeftPx;
                        break;
                    }
                }
                activeDrag_ = std::make_unique<TabReorderDrag>(
                    sx,
                    sy,
                    mb,
                    platform_,
                    mouseCtx_.tabBarClickBarId,
                    stackId,
                    draggedTabId,
                    mouseCtx_.tabBarClickIndex,
                    threshold,
                    dragOffsetXInTab);
            }
        }

        // Sub-bar clicks: resolve barId+index to the Stack-child Uuid and
        // dispatch ActivateTab / CloseTab with `target` set. The executor
        // handles top-level and sub-stack uniformly. Skip the generic
        // mouse-binding dispatch below — those bindings carry index = -1
        // intended for the top-level bar.
        if (region == MouseRegion::TabBar && !mouseCtx_.tabBarClickBarId.isNil() && mouseCtx_.tabBarClickBarId != platform_->scriptEngine_.primaryTabBarNode() && mouseCtx_.tabBarClickIndex >= 0) {
            Uuid childId;
            {
                LayoutTree &tree = platform_->scriptEngine_.layoutTree();
                const Node *bar  = tree.node(mouseCtx_.tabBarClickBarId);
                if (bar) {
                    if (auto *tb = std::get_if<TabBarData>(&bar->data)) {
                        const Node *stack = tree.node(tb->boundStack);
                        if (stack) {
                            if (auto *sd = std::get_if<StackData>(&stack->data)) {
                                if (mouseCtx_.tabBarClickIndex < static_cast<int>(sd->children.size())) {
                                    childId = sd->children[mouseCtx_.tabBarClickIndex].id;
                                }
                            }
                        }
                    }
                }
            }
            if (childId.isNil()) {
                return;
            }
            if (mb == MouseButton::Left) {
                platform_->actionRouter_->dispatch(
                    Action::ActivateTab { childId, -1 });
            } else if (mb == MouseButton::Middle) {
                platform_->actionRouter_->dispatch(
                    Action::CloseTab { childId, -1 });
            }
            return;
        }

        for (MouseBindingResult &res : matched) {
            std::visit(overloaded { [&](Action::Any &act)
                                    {
                                        // Resolve ActivateTab{-1} / CloseTab{-1} using tab bar click index
                                        if (auto *at = std::get_if<Action::ActivateTab>(&act)) {
                                            if (at->index == -1) {
                                                at->index = mouseCtx_.tabBarClickIndex;
                                            }
                                            if (at->index < 0) {
                                                return; // no tab hit
                                            }
                                        }
                                        if (auto *ct = std::get_if<Action::CloseTab>(&act)) {
                                            if (ct->index == -1) {
                                                ct->index = mouseCtx_.tabBarClickIndex;
                                            }
                                            if (ct->index < 0) {
                                                return; // no tab hit
                                            }
                                        }

                                        // SelectCommandOutput on the mouse path: hit-test the
                                        // clicked row to find its command and convert the output
                                        // range into a text selection. Falling through to the
                                        // generic dispatch would instead use the viewport-center
                                        // heuristic, which is wrong for a click.
                                        if (std::holds_alternative<Action::SelectCommandOutput>(act)) {
                                            // Document line-id resolution + command lookup are
                                            // parse-mutated. Lock for both. selectCommandOutputForRecord
                                            // takes mMutex internally (recursive, no problem).
                                            std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                                            if (!term->usingAltScreen() && cellRow >= 0 && cellRow < term->height()) {
                                                int absRow      = term->document().historySize() - term->viewportOffset() + cellRow;
                                                uint64_t lineId = term->document().lineIdForAbs(absRow);
                                                const auto *rec = term->commandForLineId(lineId);
                                                if (rec) {
                                                    term->selectCommandOutputForRecord(rec);
                                                }
                                            }
                                            return;
                                        }

                                        // PasteSelection: X11 primary selection on Linux, clipboard fallback elsewhere.
                                        // Async — fire convert_selection, return immediately. The callback
                                        // lands when SELECTION_NOTIFY arrives (handleSelectionNotify) or when
                                        // the deadline expires (sweepStaleSelectionRequests). Re-resolves the
                                        // terminal by nodeId at completion in case the pane was closed mid-flight.
                                        if (std::holds_alternative<Action::PasteSelection>(act)) {
                                            auto *t = dynamic_cast<Terminal *>(term);
                                            if (!t || !window) {
                                                return;
                                            }
                                            const Uuid nodeId      = t->nodeId();
                                            auto pasteIfStillAlive = [this, nodeId](std::optional<std::string> text)
                                            {
                                                if (!text || text->empty()) {
                                                    return;
                                                }
                                                if (auto *tt = platform_->scriptEngine_.terminal(nodeId)) {
                                                    tt->pasteText(*text);
                                                }
                                            };
                                            window->requestSelection(Window::SelectionSource::Primary,
                                                                     [this, nodeId, paste = pasteIfStillAlive](std::optional<std::string> text)
                                                                     {
                                                                         if (text && !text->empty()) {
                                                                             paste(std::move(text));
                                                                             return;
                                                                         }
                                                                         // Primary empty/refused/timed-out → fall back to clipboard.
                                                                         if (Window *w = platform_->window_.get()) {
                                                                             w->requestSelection(Window::SelectionSource::Clipboard, paste);
                                                                         }
                                                                     });
                                            return;
                                        }

                                        platform_->dispatchAction(act);
                                    },
                                    [&](MouseAction::Any &mact)
                                    {
                                        // StartSelection: dispatch based on selection type
                                        if (auto *ss = std::get_if<MouseAction::StartSelection>(&mact)) {
                                            int col = mouseCtx_.cellCol, row = mouseCtx_.cellRow;
                                            // Document line-id resolution + selection mutators
                                            // are all parse-mutated state. Take the lock once
                                            // for the whole switch (recursive — inner mutators
                                            // re-lock, which is fine).
                                            std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                                            int absRow = term->document().historySize() - term->viewportOffset() + row;

                                            // Wezterm-style boundary anchor: clicks past the cell
                                            // midpoint snap to the next boundary, so selection start
                                            // and "trailing" end exclude the click cell when the click
                                            // is closer to the cell edge than its center.
                                            bool xRightHalf = (cellRelX - cellCol * charWidth) >= (charWidth * 0.5);
                                            switch (ss->type) {
                                                case MouseAction::SelectionType::Normal: {
                                                    // Arm pending selection via mousePressEvent
                                                    MouseEvent ev;
                                                    ev.x          = col;
                                                    ev.y          = row;
                                                    ev.globalX    = static_cast<int>(sx);
                                                    ev.globalY    = static_cast<int>(sy);
                                                    ev.pixelX     = static_cast<int>(cellRelX);
                                                    ev.pixelY     = static_cast<int>(cellRelY);
                                                    ev.xRightHalf = xRightHalf;
                                                    ev.modifiers  = lastMods_;
                                                    ev.button     = LeftButton;
                                                    ev.buttons    = ev.button;
                                                    term->mousePressEvent(&ev);
                                                    activeDrag_ = std::make_unique<SelectionDrag>(sx, sy, MouseButton::Left, term);
                                                    break;
                                                }
                                                case MouseAction::SelectionType::Word:
                                                    term->startWordSelection(col, absRow);
                                                    // Selection stays `active` so a follow-up drag
                                                    // calls into `updateSelection` (mode-aware) and
                                                    // extends word-by-word. Release finalizes via the
                                                    // existing SelectionDrag path above.
                                                    activeDrag_ = std::make_unique<SelectionDrag>(sx, sy, MouseButton::Left, term);
                                                    activeDrag_->setStarted(true);
#if defined(__linux__)
                                                    if (term->hasSelection()) {
                                                        auto s = term->selectedText();
                                                        if (!s.empty())
                                                            if (window)
                                                                window->setPrimarySelection(s);
                                                    }
#endif
                                                    break;
                                                case MouseAction::SelectionType::Line:
                                                    term->startLineSelection(absRow);
                                                    activeDrag_ = std::make_unique<SelectionDrag>(sx, sy, MouseButton::Left, term);
                                                    activeDrag_->setStarted(true);
#if defined(__linux__)
                                                    if (term->hasSelection()) {
                                                        auto s = term->selectedText();
                                                        if (!s.empty())
                                                            if (window)
                                                                window->setPrimarySelection(s);
                                                    }
#endif
                                                    break;
                                                case MouseAction::SelectionType::Extend:
                                                    term->extendSelection(col, absRow, xRightHalf);
#if defined(__linux__)
                                                    if (term->hasSelection()) {
                                                        auto s = term->selectedText();
                                                        if (!s.empty())
                                                            if (window)
                                                                window->setPrimarySelection(s);
                                                    }
#endif
                                                    break;
                                                case MouseAction::SelectionType::Rectangle:
                                                    term->startRectangleSelection(col, absRow, xRightHalf);
                                                    activeDrag_ = std::make_unique<SelectionDrag>(sx, sy, MouseButton::Left, term);
                                                    break;
                                            }
                                            return;
                                        }

                                        // OpenHyperlink: resolve hyperlink at cell position; no-op when
                                        // no URL is present. Command selection is a separate action.
                                        if (std::holds_alternative<MouseAction::OpenHyperlink>(mact)) {
                                            int col = cellCol, row = cellRow;
                                            if (col >= 0 && col < term->width() && row >= 0 && row < term->height()) {
                                                // Grid + hyperlink registry parse-mutated;
                                                // copy uri out under the lock so platformOpenURL
                                                // (potentially blocking) runs unlocked.
                                                std::string uri;
                                                {
                                                    std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                                                    const CellExtra *extra = term->grid().getExtra(col, row);
                                                    if (extra && extra->hyperlinkId) {
                                                        const std::string *p = term->hyperlinkURI(extra->hyperlinkId);
                                                        if (p)
                                                            uri = *p;
                                                    }
                                                }
                                                if (!uri.empty())
                                                    platformOpenURL(uri);
                                            }
                                            return;
                                        }

                                        // SelectCommand: set OSC 133 highlight to the command containing
                                        // this row (or clear if none). Skipped on alt screen.
                                        if (std::holds_alternative<MouseAction::SelectCommand>(mact)) {
                                            std::lock_guard<std::recursive_mutex> _lk(term->mutex());
                                            if (!term->usingAltScreen() && cellRow >= 0 && cellRow < term->height()) {
                                                int absRow      = term->document().historySize() - term->viewportOffset() + cellRow;
                                                uint64_t lineId = term->document().lineIdForAbs(absRow);
                                                const auto *rec = term->commandForLineId(lineId);
                                                term->setSelectedCommand(rec ? std::optional<uint64_t> { rec->id } : std::nullopt);
                                            }
                                            return;
                                        }
                                    } },
                       res);
        }
        return;
    }

    // No binding match — if grabbed, forward to terminal's mouse reporting.
    // Skip when the click landed on a divider — the pane shouldn't see it.
    if (mode == MouseMode::Grabbed && region == MouseRegion::Pane) {
        Rect pr         = fp ? fp->rect() : Rect { 0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight) };
        double cellRelX = sx - pr.x - padLeft;
        double cellRelY = sy - pr.y - padTop;

        MouseEvent ev;
        ev.x         = static_cast<int>(cellRelX / charWidth);
        ev.y         = static_cast<int>(cellRelY / lineHeight);
        ev.globalX   = static_cast<int>(sx);
        ev.globalY   = static_cast<int>(sy);
        ev.pixelX    = static_cast<int>(cellRelX);
        ev.pixelY    = static_cast<int>(cellRelY);
        ev.modifiers = lastMods_;
        switch (button) {
            case static_cast<int>(LeftButton): ev.button = LeftButton; break;
            case static_cast<int>(MidButton): ev.button = MidButton; break;
            case static_cast<int>(RightButton): ev.button = RightButton; break;
            default: ev.button = NoButton; break;
        }
        ev.buttons = ev.button;

        if (action == static_cast<int>(KeyAction_Press)) {
            term->mousePressEvent(&ev);
        } else {
            term->mouseReleaseEvent(&ev);
        }
    }
}

void InputController::onCursorPos(double x, double y)
{
    std::lock_guard<std::recursive_mutex> plk(platform_->renderThread_->mutex());
    auto tab = platform_->activeTab();
    if (!tab) {
        return;
    }
    Script::Engine &eng = platform_->scriptEngine_;

    Window *window         = platform_->window_.get();
    Terminal *fp           = eng.focusedTerminalInSubtree(*tab);
    TerminalEmulator *term = static_cast<TerminalEmulator *>(fp);
    if (!term) {
        return;
    }
    const float contentScaleX = platform_->contentScaleX_;
    const float contentScaleY = platform_->contentScaleY_;
    const float charWidth     = platform_->charWidth_;
    const float lineHeight    = platform_->lineHeight_;
    const float padLeft       = platform_->padLeft_;
    const float padTop        = platform_->padTop_;
    const uint32_t fbWidth    = platform_->fbWidth_;
    const uint32_t fbHeight   = platform_->fbHeight_;

    lastCursorX_ = x;
    lastCursorY_ = y;
    double sx    = x * contentScaleX;
    double sy    = y * contentScaleY;

    // Update mouse cursor shape based on region. Inside a pane, honour the
    // OSC 22 pointer shape of whichever pane the mouse is physically over
    // (not necessarily the focused one) — matches user intuition that a pane
    // showing a clickable region uses a hand cursor when hovered. Falls back
    // to the I-beam for selection. The tab bar always uses the arrow.
    if (window) {
        MouseRegion region = hitTest(sx, sy);
        if (region == MouseRegion::TabBar) {
            window->setCursorStyle(Window::CursorStyle::Arrow);
        } else if (region == MouseRegion::Divider) {
            // Distinguish vertical vs horizontal divider strips by aspect:
            // w < h → vertical line between left/right panes → horizontal
            // resize arrows; h < w → horizontal line between top/bottom
            // panes → vertical resize arrows.
            Window::CursorStyle cs = Window::CursorStyle::Arrow;
            const int divPx        = eng.dividerPixels();
            if (divPx > 0) {
                const int pad = std::max(0, platform_->lastConfig().divider_hit_pad);
                for (const auto &r : eng.tabDividerRects(*tab, divPx)) {
                    if (sx >= r.x - pad && sx < r.x + r.w + pad && sy >= r.y - pad && sy < r.y + r.h + pad) {
                        cs = (r.w < r.h) ? Window::CursorStyle::ResizeH : Window::CursorStyle::ResizeV;
                        break;
                    }
                }
            }
            window->setCursorStyle(cs);
        } else if (wouldOpenHyperlinkAt(sx, sy)) {
            window->setCursorStyle(Window::CursorStyle::Pointer);
        } else {
            Uuid hoveredId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
            auto it        = paneCursorStyle_.find(hoveredId);
            window->setCursorStyle(it != paneCursorStyle_.end()
                                       ? it->second
                                       : Window::CursorStyle::IBeam);
        }
    }

    // Notify JS mousemove listeners for the hovered pane — and, when the
    // cursor is inside a popup or embedded on that pane, for those too.
    // Popup/embedded takes precedence (it's "on top of" the pane visually).
    Uuid hoveredPaneId = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    Terminal *hp       = hoveredPaneId.isNil() ? nullptr : eng.paneInSubtree(*tab, hoveredPaneId);
    if (hp) {
        Rect hpr   = hp->rect();
        double hcx = sx - hpr.x - padLeft;
        double hcy = sy - hpr.y - padTop;

        bool routedToChild = false;
        // Popup first (on top in the z-order).
        if (!routedToChild) {
            int cellCol = static_cast<int>(hcx / charWidth);
            int cellRow = static_cast<int>(hcy / lineHeight);
            for (const auto &popup : hp->popups()) {
                if (cellCol >= popup->cellX() && cellCol < popup->cellX() + popup->cellW() &&
                    cellRow >= popup->cellY() && cellRow < popup->cellY() + popup->cellH()) {
                    int relPxX = static_cast<int>(hcx) - popup->cellX() * static_cast<int>(charWidth);
                    int relPxY = static_cast<int>(hcy) - popup->cellY() * static_cast<int>(lineHeight);
                    platform_->scriptEngine_.deliverPopupMouseMove(
                        hp->id(),
                        popup->popupId(),
                        cellCol - popup->cellX(),
                        cellRow - popup->cellY(),
                        relPxX,
                        relPxY);
                    routedToChild = true;
                    break;
                }
            }
        }
        // Then embedded.
        if (!routedToChild && !hp->usingAltScreen() && hp->hasEmbeddeds()) {
            uint64_t hitLineId = 0;
            int emRelCol = 0, emRelRow = 0, emRelPx = 0, emRelPy = 0;
            if (hp->liveSegmentHitTest(hcx, hcy, static_cast<float>(charWidth), lineHeight, hitLineId, emRelCol, emRelRow, emRelPx, emRelPy)) {
                platform_->scriptEngine_.deliverEmbeddedMouseMove(
                    hp->id(),
                    hitLineId,
                    emRelCol,
                    emRelRow,
                    emRelPx,
                    emRelPy);
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

    Rect pr         = fp ? fp->rect() : Rect { 0, 0, static_cast<int>(fbWidth), static_cast<int>(fbHeight) };
    // relX/relY: pane-relative pixels (used for pane bounds tests).
    // cellRelX/cellRelY: cell-grid-relative pixels (used for cell math + pixel reporting).
    double relX     = sx - pr.x;
    double relY     = sy - pr.y;
    double cellRelX = relX - padLeft;
    double cellRelY = relY - padTop;

    // Helper to build a MouseEvent from current state
    auto buildMouseEvent = [&]()
    {
        MouseEvent ev;
        ev.x          = static_cast<int>(cellRelX / charWidth);
        ev.y          = static_cast<int>(cellRelY / lineHeight);
        ev.globalX    = static_cast<int>(sx);
        ev.globalY    = static_cast<int>(sy);
        ev.pixelX     = static_cast<int>(cellRelX);
        ev.pixelY     = static_cast<int>(cellRelY);
        ev.xRightHalf = (cellRelX - ev.x * charWidth) >= (charWidth * 0.5);
        ev.button     = NoButton;
        ev.modifiers  = lastMods_;
        if ((heldButtons_ & LeftButton) != 0) {
            ev.buttons |= LeftButton;
        }
        if ((heldButtons_ & MidButton) != 0) {
            ev.buttons |= MidButton;
        }
        if ((heldButtons_ & RightButton) != 0) {
            ev.buttons |= RightButton;
        }
        return ev;
    };

    // 1a. Tab-bar drag-to-reorder. Threshold-gated: bare press + small jitter
    // doesn't enter drag mode (so a normal click on a tab still activates).
    // Once `started_`, each cursor motion hit-tests against the current strip
    // and calls LayoutTree::moveChild when the cursor crosses into another
    // tab's slot — the dragged tab slides to the new position next frame.
    // Drops out of this branch when activeDrag_ is some other kind.
    if (auto *td = dynamic_cast<TabReorderDrag *>(activeDrag_.get())) {
        if (!td->started()) {
            double dx     = sx - td->originX();
            int threshold = td->thresholdPx();
            if (std::abs(dx) < static_cast<double>(threshold)) {
                return;
            }
            td->setStarted(true);
            platform_->setDraggedTab(td->barId(), td->draggedTabId(), static_cast<float>(sx), td->dragOffsetXInTab());
        } else {
            platform_->setDragCursorX(static_cast<float>(sx));
        }
        // Midpoint-trigger reorder, Chrome-style. Swap with a neighbor only
        // when the cursor crosses *that neighbor's* midpoint, not when the
        // cursor moves into the neighbor's range. With asymmetric tab widths
        // an edge-based test causes dither: the dragged tab swaps into a
        // wider neighbor's range, the wider neighbor's new range straddles
        // the cursor in the opposite direction, and the next motion swaps
        // back. Midpoint-based has hysteresis — once swapped, the cursor is
        // on the just-vacated side of the trigger line, so further small
        // motions don't oscillate. One swap max per motion event; fast
        // cursor motion catches up over a few events. Y is ignored so the
        // strip still slides when the cursor wanders above/below the bar.
        int currentIdx        = td->currentIndex();
        int newIdx            = currentIdx;
        const auto &bars      = platform_->renderThread_->renderState().tabBars;
        float tabBarCharWidth = platform_->tabBarCharWidth_;
        for (const TabBarRender &br : bars) {
            if (br.id != td->barId()) {
                continue;
            }
            auto midpointOf = [&](int idx) -> std::optional<float>
            {
                if (idx < 0 || idx >= static_cast<int>(br.colRanges.size())) {
                    return std::nullopt;
                }
                int s = br.colRanges[idx].first;
                int e = br.colRanges[idx].second;
                if (s < 0 || e <= s) {
                    return std::nullopt;
                }
                return static_cast<float>(br.rect.x) + (static_cast<float>(s + e) / 2.0f) * tabBarCharWidth;
            };
            if (auto leftMid = midpointOf(currentIdx - 1); leftMid && sx < *leftMid) {
                newIdx = currentIdx - 1;
            } else if (auto rightMid = midpointOf(currentIdx + 1); rightMid && sx > *rightMid) {
                newIdx = currentIdx + 1;
            }
            break;
        }
        if (newIdx != currentIdx) {
            int delta = newIdx - currentIdx;
            if (platform_->scriptEngine_.layoutTree().moveChild(td->stackId(), td->draggedTabId(), delta)) {
                td->setCurrentIndex(newIdx);
                platform_->tabBarDirty_ = true;
                platform_->setNeedsRedraw();
            }
        }
        return;
    }

    // 1b. Divider resize drag. Compute incremental pixel delta along the
    // drag's axis and apply via resizeTabPaneEdge. Per-event deltas avoid
    // integral drift (the resize helper adds to current stretch hints, not
    // to a captured-at-press-time baseline).
    if (auto *dr = dynamic_cast<DividerResizeDrag *>(activeDrag_.get())) {
        int delta = (dr->axis() == SplitDir::Horizontal)
            ? static_cast<int>(std::lround(sx - dr->lastSx()))
            : static_cast<int>(std::lround(sy - dr->lastSy()));
        if (delta != 0) {
            if (platform_->scriptEngine_.resizeTabPaneEdge(
                    dr->subtreeRoot(),
                    dr->ownerPaneId(),
                    dr->axis(),
                    delta)) {
                // dividersDirty_ (member) is what reaches renderState_ via
                // buildRenderFrameState's merge; pending_.dividersDirty set
                // by refreshDividers (called later, from runLayoutIfDirty)
                // gets wiped by pending_.clear() before snapshotUnderLock.
                // Without this, the renderer skips the divider-VB update
                // path and dividers render at stale geometry. Same pattern
                // as Action::SwapPane (Platform_Actions.cpp:284).
                platform_->dividersDirty_ = true;
                platform_->setNeedsRedraw();
            }
            dr->setLast(sx, sy);
        }
        if (window) {
            window->setCursorStyle(dr->cursorStyle());
        }
        return;
    }

    // 1. If a selection drag is active, forward directly to terminal.
    // (Other DragHandler kinds are handled inline by their owners; selection
    // motion stays here until the second drag kind forces extraction into
    // SelectionDrag::onMotion.)
    if (auto *sel = dynamic_cast<SelectionDrag *>(activeDrag_.get())) {
        // Require movement beyond a threshold before activating the pending
        // selection, so that subpixel jitter during a click doesn't start one.
        if (!sel->started()) {
            double dx = sx - sel->originX();
            double dy = sy - sel->originY();
            if (dx * dx + dy * dy < 9.0) { // 3px threshold
                return;
            }
            sel->setStarted(true);
        }
        MouseEvent ev = buildMouseEvent();
        term->mouseMoveEvent(&ev);
        // Clear drag active on release (no buttons held)
        if (ev.buttons == 0) {
            activeDrag_.reset();
            stopAutoScroll();
            return;
        }
        // Auto-scroll when mouse is outside the pane's vertical bounds.
        // Snapshot viewport vs history-size pair under the parse lock.
        double paneH = pr.h;
        int vp;
        int hist;
        {
            std::lock_guard<std::recursive_mutex> _lk(term->mutex());
            vp   = term->viewportOffset();
            hist = term->document().historySize();
        }
        // Hot zone: a single cell row at the top/bottom of the pane. Inside
        // this band the autoscroll timer arms; velocity is purely a function
        // of how long the timer has been running (see doAutoScroll). A one-row
        // band is the minimum needed for maximized/fullscreen panes, where the
        // OS clips the cursor to the screen so `relY` can never go past the
        // pane boundary.
        const float hotZone = lineHeight;
        bool above          = relY < hotZone;
        bool below          = relY >= paneH - hotZone;
        if (above && vp < hist) {
            startAutoScroll(+1, ev.x);
        } else if (below && vp > 0) {
            startAutoScroll(-1, ev.x);
        } else {
            stopAutoScroll();
        }
        return;
    }

    // 2. Check if any button is held
    bool buttonHeld = ((heldButtons_ & LeftButton) != 0) || ((heldButtons_ & MidButton) != 0) || ((heldButtons_ & RightButton) != 0);

    if (buttonHeld) {
        // 3. Feed to click detector — may produce a Drag event
        auto dragResult = clickDetector_.onMove(static_cast<int>(sx), static_cast<int>(sy));
        if (dragResult && dragResult->type == MouseEventType::Drag) {
            MouseMode mode     = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;
            MouseRegion region = hitTest(sx, sy);

            MouseStroke stroke;
            stroke.button = dragResult->button;
            stroke.mods   = lastMods_ & kBindingModMask;
            stroke.event  = MouseEventType::Drag;
            stroke.mode   = mode;
            stroke.region = region;

            auto matched = matchMouseBindings(stroke, mouseBindings_);
            if (!matched.empty()) {
                // Drag bindings are rare and overlapping ones on the same
                // stroke don't compose cleanly — use the first match only.
                MouseBindingResult &res = matched.front();
                std::visit(overloaded { [&](Action::Any &act)
                                        {
                                            platform_->dispatchAction(act);
                                        },
                                        [&](MouseAction::Any &mact)
                                        {
                                            if (std::holds_alternative<MouseAction::StartSelection>(mact)) {
                                                MouseEvent ev = buildMouseEvent();
                                                term->mousePressEvent(&ev);
                                                activeDrag_ = std::make_unique<SelectionDrag>(sx, sy, dragResult->button, term);
                                                activeDrag_->setStarted(true); // drag detector already passed threshold
                                            }
                                            // OpenHyperlink/SelectCommand don't begin drags; ignore on Drag event.
                                        } },
                           res);
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
        // 4. No button held — forward motion to the pane under the cursor
        // (hover/tracking). Not the focused pane: a focused app with
        // any-motion tracking must not receive reports (with out-of-range
        // coordinates, at that) while the cursor is over a different split.
        if (auto *ht = static_cast<TerminalEmulator *>(hp)) {
            Rect hpr      = hp->rect();
            // Clamp to 0: inside the pane's padding the cell-grid-relative
            // position is slightly negative, which would knock SGR-pixel
            // reporting (1016) down to cell format.
            double hRelX  = std::max(0.0, sx - hpr.x - padLeft);
            double hRelY  = std::max(0.0, sy - hpr.y - padTop);
            MouseEvent ev = buildMouseEvent();
            ev.x          = static_cast<int>(hRelX / charWidth);
            ev.y          = static_cast<int>(hRelY / lineHeight);
            ev.pixelX     = static_cast<int>(hRelX);
            ev.pixelY     = static_cast<int>(hRelY);
            ev.xRightHalf = (hRelX - ev.x * charWidth) >= (charWidth * 0.5);
            ht->mouseMoveEvent(&ev);
        }
    }
}

void InputController::startAutoScroll(int dir, int col)
{
    // Fresh start or direction flip resets the ramp; otherwise the in-flight
    // velocity is preserved so the timer body can keep ramping.
    if (autoScrollDir_ != dir) {
        autoScrollLinesPerTick_ = 1;
        autoScrollTickCount_    = 0;
    }
    autoScrollDir_ = dir;
    autoScrollCol_ = col;
    if (autoScrollTimerActive_) {
        return; // already running, dir/col updated above
    }
    EventLoop *el = platform_->eventLoop_.get();
    if (!el) {
        return;
    }
    autoScrollTimer_       = el->addTimer(50, true, [this]()
                                    {
                                        doAutoScroll();
                                    });
    autoScrollTimerActive_ = true;
}

void InputController::stopAutoScroll()
{
    if (!autoScrollTimerActive_) {
        return;
    }
    if (EventLoop *el = platform_->eventLoop_.get()) {
        el->removeTimer(autoScrollTimer_);
    }
    autoScrollTimerActive_  = false;
    autoScrollLinesPerTick_ = 1;
    autoScrollTickCount_    = 0;
    autoScrollDir_          = 0;
}

void InputController::doAutoScroll()
{
    // Only the selection drag drives auto-scroll today; future drags can opt
    // in by checking activeDrag_ kind here when they need it.
    if (!dynamic_cast<SelectionDrag *>(activeDrag_.get())) {
        stopAutoScroll();
        return;
    }

    auto tab = platform_->activeTab();
    if (!tab) {
        stopAutoScroll();
        return;
    }
    Terminal *fp           = platform_->scriptEngine_.focusedTerminalInSubtree(*tab);
    TerminalEmulator *term = static_cast<TerminalEmulator *>(fp);
    if (!term) {
        stopAutoScroll();
        return;
    }

    term->scrollViewport(autoScrollDir_ * autoScrollLinesPerTick_);

    // Ramp velocity: +1 line/tick every 8 ticks (400 ms at the 50 ms tick),
    // capped at 8 lines/tick. Reaches the cap after ~2.8 s.
    constexpr int kRampEveryNTicks = 8;
    constexpr int kMaxLinesPerTick = 8;
    if (++autoScrollTickCount_ >= kRampEveryNTicks) {
        autoScrollTickCount_ = 0;
        if (autoScrollLinesPerTick_ < kMaxLinesPerTick) {
            ++autoScrollLinesPerTick_;
        }
    }

    // Extend selection to the boundary row now visible
    MouseEvent ev;
    ev.x         = autoScrollCol_;
    ev.y         = (autoScrollDir_ > 0) ? 0 : term->height() - 1;
    ev.globalX   = 0;
    ev.globalY   = 0;
    ev.button    = NoButton;
    ev.buttons   = LeftButton;
    ev.modifiers = lastMods_;
    term->mouseMoveEvent(&ev);

    // Stop if there's no more scrollback to consume. Snapshot under
    // lock so viewport vs. history-size read pair is consistent.
    int vp;
    int hist;
    {
        std::lock_guard<std::recursive_mutex> _lk(term->mutex());
        vp   = term->viewportOffset();
        hist = term->document().historySize();
    }
    if (autoScrollDir_ > 0 && vp >= hist) {
        stopAutoScroll();
    } else if (autoScrollDir_ < 0 && vp == 0) {
        stopAutoScroll();
    }

    platform_->setNeedsRedraw();
}

bool InputController::wouldOpenHyperlinkAt(double sx, double sy)
{
    if (!platform_) {
        return false;
    }
    auto tab = platform_->activeTab();
    if (!tab) {
        return false;
    }
    Script::Engine &eng = platform_->scriptEngine_;
    Uuid hoveredId      = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    if (hoveredId.isNil()) {
        return false;
    }
    Terminal *hp = eng.paneInSubtree(*tab, hoveredId);
    auto *term   = dynamic_cast<TerminalEmulator *>(hp);
    if (!term) {
        return false;
    }

    const float charWidth  = platform_->charWidth_;
    const float lineHeight = platform_->lineHeight_;
    const float padLeft    = platform_->padLeft_;
    const float padTop     = platform_->padTop_;
    if (charWidth <= 0.0f || lineHeight <= 0.0f) {
        return false;
    }

    Rect pr     = hp->rect();
    double relX = sx - pr.x - padLeft;
    double relY = sy - pr.y - padTop;
    if (relX < 0 || relY < 0) {
        return false;
    }
    int col = static_cast<int>(relX / charWidth);
    int row = static_cast<int>(relY / lineHeight);
    if (col < 0 || col >= term->width() || row < 0 || row >= term->height()) {
        return false;
    }

    // Grid + hyperlink registry parse-mutated; lock for the lookup.
    std::lock_guard<std::recursive_mutex> _lk(term->mutex());
    const CellExtra *extra = term->grid().getExtra(col, row);
    if (!extra || !extra->hyperlinkId) {
        return false;
    }
    const std::string *uri = term->hyperlinkURI(extra->hyperlinkId);
    if (!uri || uri->empty()) {
        return false;
    }

    MouseStroke stroke;
    stroke.button = MouseButton::Left;
    stroke.mods   = lastMods_ & kBindingModMask;
    stroke.event  = MouseEventType::Click;
    stroke.mode   = term->mouseReportingActive() ? MouseMode::Grabbed : MouseMode::Ungrabbed;
    stroke.region = MouseRegion::Pane;
    auto matched  = matchMouseBindings(stroke, mouseBindings_);
    for (const auto &res : matched) {
        if (auto *mact = std::get_if<MouseAction::Any>(&res)) {
            if (std::holds_alternative<MouseAction::OpenHyperlink>(*mact)) {
                return true;
            }
        }
    }
    return false;
}

void InputController::refreshPointerShape()
{
    Window *window = platform_->window_.get();
    if (!window || platform_->isHeadless()) {
        return;
    }
    auto tab = platform_->activeTab();
    if (!tab) {
        return;
    }
    // When the pointer is over the tab bar, show the arrow regardless of the
    // focused pane's cursor shape — matches the hover path in onMouseMove.
    // Without this, clicking a tab triggers a focus change and applies the
    // focused pane's IBeam even though the pointer is still in the tab bar.
    double sx          = lastCursorX_ * platform_->contentScaleX_;
    double sy          = lastCursorY_ * platform_->contentScaleY_;
    MouseRegion region = hitTest(sx, sy);
    if (region == MouseRegion::TabBar) {
        window->setCursorStyle(Window::CursorStyle::Arrow);
        return;
    }
    if (region == MouseRegion::Divider) {
        Script::Engine &eng    = platform_->scriptEngine_;
        Window::CursorStyle cs = Window::CursorStyle::Arrow;
        const int divPx        = eng.dividerPixels();
        if (divPx > 0) {
            const int pad = std::max(0, platform_->lastConfig().divider_hit_pad);
            for (const auto &r : eng.tabDividerRects(*tab, divPx)) {
                if (sx >= r.x - pad && sx < r.x + r.w + pad && sy >= r.y - pad && sy < r.y + r.h + pad) {
                    cs = (r.w < r.h) ? Window::CursorStyle::ResizeH : Window::CursorStyle::ResizeV;
                    break;
                }
            }
        }
        window->setCursorStyle(cs);
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
    Script::Engine &eng = platform_->scriptEngine_;
    Uuid paneId         = eng.paneAtPixelInSubtree(*tab, static_cast<int>(sx), static_cast<int>(sy));
    if (paneId.isNil()) {
        if (Terminal *focPane = eng.focusedTerminalInSubtree(*tab)) {
            paneId = focPane->nodeId();
        }
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
void InputController::replayPendingSequenceKey(const PendingKey &p)
{
    TerminalEmulator *term = static_cast<TerminalEmulator *>(platform_->activeTerm());
    if (!term) {
        return;
    }
    Window *window = platform_->window_.get();

    Key k = static_cast<Key>(p.key);
    KeyEvent ev;
    ev.key       = k;
    ev.modifiers = static_cast<uint32_t>(p.mods);
    ev.action    = KeyAction_Press;
    ev.count     = 1;

    const bool ctrl = (p.mods & CtrlModifier) != 0;

    if (term->kittyFlags() != 0) {
        if (ctrl && ((p.key >= Key_A && p.key <= Key_Z) || (p.key >= 0x61 && p.key <= 0x7a))) {
            char ch = (p.key >= 0x61) ? static_cast<char>(p.key) : static_cast<char>(p.key - Key_A + 'a');
            ev.text = std::string(1, ch);
        } else if (p.key >= Key_Space && p.key <= Key_AsciiTilde) {
            std::string name = window ? window->keyName(p.scancode) : std::string {};
            if (!name.empty()) {
                ev.text = name;
            }
        }
        if (window) {
            ev.shiftedKey    = window->shiftedKeyCodepoint(p.scancode);
            ev.baseLayoutKey = window->baseLayoutKeyCodepoint(p.scancode);
        }
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy: ctrl+letter
    if (ctrl && ((p.key >= Key_A && p.key <= Key_Z) || (p.key >= 0x61 && p.key <= 0x7a))) {
        int offset = (p.key >= 0x61) ? (p.key - 0x61) : (p.key - Key_A);
        ev.text    = std::string(1, static_cast<char>(offset + 1));
        term->keyPressEvent(&ev);
        return;
    }

    // Legacy: ctrl + space/punctuation/digit (mirrors the onKey path).
    if (ctrl && window && p.key < 0x01000000 && k != Key_unknown) {
        uint32_t ch = 0;
        if ((p.mods & ShiftModifier) != 0) {
            ch = window->shiftedKeyCodepoint(p.scancode);
        }
        if (ch == 0) {
            std::string base = window->keyName(p.scancode);
            if (!base.empty()) {
                ch = static_cast<unsigned char>(base[0]);
            }
        }
        int mapped = ctrlMappingForChar(ch);
        if (mapped >= 0) {
            char outByte         = static_cast<char>(mapped);
            const bool altPrefix = (p.mods & OptionAsAltModifier) != 0;
            ev.text              = altPrefix ? std::string("\x1b") + outByte : std::string(1, outByte);
            term->keyPressEvent(&ev);
            return;
        }
    }

    // Legacy: alt+printable with altSendsEsc. Gate on OptionAsAltModifier
    // so the per-side macos_option_as_alt config carries through replay.
    if (altSendsEsc_ && (p.mods & OptionAsAltModifier) && !(p.mods & CtrlModifier) &&
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
    if (sequenceTimeoutMs_ <= 0) {
        return;
    }
    EventLoop *loop = platform_->eventLoop_.get();
    if (!loop) {
        return;
    }
    cancelSequenceTimeout();
    sequenceTimerId_ = loop->addTimer(
        static_cast<uint64_t>(sequenceTimeoutMs_),
        false,
        [this]()
        {
            onSequenceTimeout();
        });
}

void InputController::cancelSequenceTimeout()
{
    if (sequenceTimerId_ == 0) {
        return;
    }
    if (EventLoop *loop = platform_->eventLoop_.get()) {
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
    for (const auto &p : pending) {
        replayPendingSequenceKey(p);
    }
}
