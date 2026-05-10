#include "TerminalEmulator.h"
#include "Utf8.h"
#include <cstdio>
#include <spdlog/spdlog.h>

// `isModifierKey` lives in InputTypes.h so InputController.cpp can
// share it (the platform-side resetViewport/resetBlink calls have
// the same modifier-skip requirement as the emulator-side selection
// clear and viewport reset below).

void TerminalEmulator::keyPressEvent(const KeyEvent *event)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Modifier-only presses (Shift, Ctrl, Cmd, Alt alone) must NOT
    // jump the viewport back to live or wipe the selection — the
    // user is mid-gesture (e.g. Ctrl-click on a URL up in the
    // scrollback). Real keystrokes (with or without modifiers) reset
    // the viewport so typing always brings you back to the prompt;
    // bare modifier taps stay where they are.
    //
    // Bindings that match short-circuit earlier in InputController::onKey
    // and never reach here, so any event we see is either PTY-bound
    // typing or a bare modifier press we want to ignore.
    if (!isModifierKey(event->key)) {
        resetViewport();
        if (hasSelection()) {
            clearSelection();
        }
    }

    spdlog::debug("keyPressEvent: key=0x{:x} text='{}' ({} bytes) count={} mods=0x{:x} action={}",
                  static_cast<int>(event->key),
                  toPrintable(event->text),
                  event->text.size(),
                  event->count,
                  event->modifiers,
                  static_cast<int>(event->action));

    if (event->key == Key_F12) {
        return;
    }

    // Kitty keyboard protocol: encode via CSI u when flags are active
    if (mKittyFlags != 0) {
        std::string encoded = encodeKittyKey(*event);
        if (!encoded.empty()) {
            writeToOutput(encoded.c_str(), encoded.size());
        }
        return;
    }

    // Legacy encoding: drop release events
    if (event->action == KeyAction_Release) {
        return;
    }

    // xterm modifier code: 1=none, +1 shift, +2 alt, +4 ctrl, +8 meta.
    // CapsLock / NumLock are not encoded in legacy mode.
    uint32_t modCode = 1;
    if (event->modifiers & ShiftModifier) modCode += 1;
    if (event->modifiers & AltModifier)   modCode += 2;
    if (event->modifiers & CtrlModifier)  modCode += 4;
    if (event->modifiers & MetaModifier)  modCode += 8;
    const bool hasMods = (modCode != 1);

    std::string text = event->text;
    if (text.empty()) {
        char buf[32];
        // \E[1;<mods><letter> for arrow/Home/End/F1-F4 when modifiers held.
        auto letterMod = [&](char trailer) -> std::string {
            int n = snprintf(buf, sizeof(buf), "\x1b[1;%u%c", modCode, trailer);
            return std::string(buf, n);
        };
        // \E[<num>[;<mods>]~ for tilde-form keys (PgUp/Dn/Ins/Del/F5+).
        auto tildeForm = [&](int num) -> std::string {
            int n = hasMods
                ? snprintf(buf, sizeof(buf), "\x1b[%d;%u~", num, modCode)
                : snprintf(buf, sizeof(buf), "\x1b[%d~", num);
            return std::string(buf, n);
        };

        switch (event->key) {
            case Key_Return:
            case Key_Enter: text = "\r"; break;
            case Key_Backspace: text = "\x7f"; break;
            case Key_Tab:
                // Shift-Tab → CBT; other mods fall through to plain tab.
                text = (event->modifiers & ShiftModifier) ? "\x1b[Z" : "\t";
                break;
            case Key_Escape: text = "\x1b"; break;
            case Key_Delete: text = tildeForm(3); break;
            case Key_Left:
                text = hasMods ? letterMod('D') : (mState->cursorKeyMode ? "\x1bOD" : "\x1b[D");
                break;
            case Key_Right:
                text = hasMods ? letterMod('C') : (mState->cursorKeyMode ? "\x1bOC" : "\x1b[C");
                break;
            case Key_Up:
                text = hasMods ? letterMod('A') : (mState->cursorKeyMode ? "\x1bOA" : "\x1b[A");
                break;
            case Key_Down:
                text = hasMods ? letterMod('B') : (mState->cursorKeyMode ? "\x1bOB" : "\x1b[B");
                break;
            case Key_Home:
                text = hasMods ? letterMod('H') : (mState->cursorKeyMode ? "\x1bOH" : "\x1b[H");
                break;
            case Key_End:
                text = hasMods ? letterMod('F') : (mState->cursorKeyMode ? "\x1bOF" : "\x1b[F");
                break;
            case Key_PageUp:   text = tildeForm(5); break;
            case Key_PageDown: text = tildeForm(6); break;
            case Key_Insert:   text = tildeForm(2); break;
            // F1-F4: SS3 form when unmodified, CSI form when modifiers held.
            case Key_F1: text = hasMods ? letterMod('P') : "\x1bOP"; break;
            case Key_F2: text = hasMods ? letterMod('Q') : "\x1bOQ"; break;
            case Key_F3: text = hasMods ? letterMod('R') : "\x1bOR"; break;
            case Key_F4: text = hasMods ? letterMod('S') : "\x1bOS"; break;
            case Key_F5:  text = tildeForm(15); break;
            case Key_F6:  text = tildeForm(17); break;
            case Key_F7:  text = tildeForm(18); break;
            case Key_F8:  text = tildeForm(19); break;
            case Key_F9:  text = tildeForm(20); break;
            case Key_F10: text = tildeForm(21); break;
            case Key_F11: text = tildeForm(23); break;
            case Key_F12: text = tildeForm(24); break;
            default:
                break;
        }
    }
    if (text.empty()) {
        spdlog::debug("keyPressEvent: no text to send for key=0x{:x}", static_cast<int>(event->key));
    }
    if (!text.empty() && event->count) {
        for (size_t i = 0; i < event->count; ++i) {
            writeToOutput(text.c_str(), text.size());
        }
    }
}

// --- Kitty keyboard protocol ---

void TerminalEmulator::kittyPushFlags(uint8_t flags)
{
    auto *stack = mUsingAltScreen ? mKittyStackAlt : mKittyStackMain;
    auto &depth = mUsingAltScreen ? mKittyStackDepthAlt : mKittyStackDepthMain;
    if (depth < KITTY_STACK_MAX) {
        stack[depth++] = flags;
    }
    mKittyFlags = flags;
}

void TerminalEmulator::kittyPopFlags(int count)
{
    auto *stack = mUsingAltScreen ? mKittyStackAlt : mKittyStackMain;
    auto &depth = mUsingAltScreen ? mKittyStackDepthAlt : mKittyStackDepthMain;
    for (int i = 0; i < count && depth > 0; i++) {
        depth--;
    }
    mKittyFlags = (depth > 0) ? stack[depth - 1] : 0;
}

void TerminalEmulator::kittySetFlags(uint8_t flags, int mode)
{
    switch (mode) {
        case 1: mKittyFlags = flags; break;   // replace
        case 2: mKittyFlags |= flags; break;  // OR
        case 3: mKittyFlags &= ~flags; break; // AND NOT
        default: mKittyFlags = flags; break;
    }
}

void TerminalEmulator::kittyQueryFlags()
{
    char response[16];
    int len = snprintf(response, sizeof(response), "\x1b[?%uu", mKittyFlags);
    writeToOutput(response, len);
}

// Map internal Key enum to Kitty functional key code.
// Returns 0 if the key is a printable character (use codepoint instead).
static uint32_t kittyFunctionalCode(Key k)
{
    switch (k) {
        case Key_Escape: return 27;
        case Key_Return:
        case Key_Enter: return 13;
        case Key_Tab: return 9;
        case Key_Backspace: return 127;
        case Key_Insert: return 57348;
        case Key_Delete: return 57349;
        case Key_Left: return 57350;
        case Key_Right: return 57351;
        case Key_Up: return 57352;
        case Key_Down: return 57353;
        case Key_PageUp: return 57354;
        case Key_PageDown: return 57355;
        case Key_Home: return 57356;
        case Key_End: return 57357;
        case Key_CapsLock: return 57358;
        case Key_ScrollLock: return 57359;
        case Key_NumLock: return 57360;
        case Key_Print: return 57361;
        case Key_Pause: return 57362;
        case Key_Menu: return 57363;
        case Key_F1: return 57364;
        case Key_F2: return 57365;
        case Key_F3: return 57366;
        case Key_F4: return 57367;
        case Key_F5: return 57368;
        case Key_F6: return 57369;
        case Key_F7: return 57370;
        case Key_F8: return 57371;
        case Key_F9: return 57372;
        case Key_F10: return 57373;
        case Key_F11: return 57374;
        case Key_F12: return 57375;
        case Key_F13: return 57376;
        case Key_F14: return 57377;
        case Key_F15: return 57378;
        case Key_F16: return 57379;
        case Key_F17: return 57380;
        case Key_F18: return 57381;
        case Key_F19: return 57382;
        case Key_F20: return 57383;
        case Key_F21: return 57384;
        case Key_F22: return 57385;
        case Key_F23: return 57386;
        case Key_F24: return 57387;
        case Key_F25: return 57388;
        case Key_F26: return 57389;
        case Key_F27: return 57390;
        case Key_F28: return 57391;
        case Key_F29: return 57392;
        case Key_F30: return 57393;
        case Key_F31: return 57394;
        case Key_F32: return 57395;
        case Key_F33: return 57396;
        case Key_F34: return 57397;
        case Key_F35: return 57398;
        case Key_KP_0: return 57399;
        case Key_KP_1: return 57400;
        case Key_KP_2: return 57401;
        case Key_KP_3: return 57402;
        case Key_KP_4: return 57403;
        case Key_KP_5: return 57404;
        case Key_KP_6: return 57405;
        case Key_KP_7: return 57406;
        case Key_KP_8: return 57407;
        case Key_KP_9: return 57408;
        case Key_KP_Decimal: return 57409;
        case Key_KP_Divide: return 57410;
        case Key_KP_Multiply: return 57411;
        case Key_KP_Subtract: return 57412;
        case Key_KP_Add: return 57413;
        case Key_KP_Enter: return 57414;
        case Key_KP_Equal: return 57415;
        case Key_KP_Separator: return 57416;
        case Key_KP_Left: return 57417;
        case Key_KP_Right: return 57418;
        case Key_KP_Up: return 57419;
        case Key_KP_Down: return 57420;
        case Key_KP_PageUp: return 57421;
        case Key_KP_PageDown: return 57422;
        case Key_KP_Home: return 57423;
        case Key_KP_End: return 57424;
        case Key_KP_Insert: return 57425;
        case Key_KP_Delete: return 57426;
        case Key_KP_Begin: return 57427;
        // Media keys (kitty 57428–57440)
        case Key_MediaPlay: return 57428;
        case Key_MediaPause: return 57429;
        case Key_MediaTogglePlayPause: return 57430;
        case Key_MediaReverse: return 57431;
        case Key_MediaStop: return 57432;
        case Key_MediaFastForward: return 57433;
        case Key_MediaRewind: return 57434;
        case Key_MediaNext: return 57435;
        case Key_MediaPrevious: return 57436;
        case Key_MediaRecord: return 57437;
        case Key_VolumeDown: return 57438;
        case Key_VolumeUp: return 57439;
        case Key_VolumeMute: return 57440;
        // Modifier-only keys (kitty 57441–57452)
        case Key_Shift_L: return 57441;
        case Key_Control_L: return 57442;
        case Key_Alt_L: return 57443;
        case Key_Super_L: return 57444;
        case Key_Hyper_L: return 57445;
        case Key_Meta_L: return 57446;
        case Key_Shift_R: return 57447;
        case Key_Control_R: return 57448;
        case Key_Alt_R: return 57449;
        case Key_Super_R: return 57450;
        case Key_Hyper_R: return 57451;
        case Key_Meta_R: return 57452;
        // ISO level shifts (kitty 57453–57454)
        case Key_AltGr: return 57453;            // ISO_Level3_Shift
        case Key_ISO_Level5_Shift: return 57454;
        default: return 0;
    }
}

// Legacy CSI trailer for keys that use the traditional CSI format
// Returns 0 if the key should use CSI u format instead
static char kittyLegacyTrailer(Key k)
{
    switch (k) {
        case Key_Up: return 'A';
        case Key_Down: return 'B';
        case Key_Right: return 'C';
        case Key_Left: return 'D';
        case Key_Home: return 'H';
        case Key_End: return 'F';
        case Key_F1: return 'P';
        case Key_F2: return 'Q';
        case Key_F3: return 'R';
        case Key_F4: return 'S';
        default: return 0;
    }
}

// Legacy CSI ~ number for keys that use CSI number ~ format
static int kittyLegacyTildeNumber(Key k)
{
    switch (k) {
        case Key_Insert: return 2;
        case Key_Delete: return 3;
        case Key_PageUp: return 5;
        case Key_PageDown: return 6;
        case Key_F5: return 15;
        case Key_F6: return 17;
        case Key_F7: return 18;
        case Key_F8: return 19;
        case Key_F9: return 20;
        case Key_F10: return 21;
        case Key_F11: return 23;
        case Key_F12: return 24;
        default: return 0;
    }
}

std::string TerminalEmulator::encodeKittyKey(const KeyEvent &ev) const
{
    bool disambiguate    = mKittyFlags & 0x01;
    bool reportEvents    = mKittyFlags & 0x02;
    bool reportAlternate = mKittyFlags & 0x04;
    bool reportAllKeys   = mKittyFlags & 0x08;
    bool reportText      = mKittyFlags & 0x10;

    // Drop release events unless REPORT_EVENT_TYPES is active.
    // Repeat events are always sent — without REPORT_EVENT_TYPES they are
    // encoded identically to press (no event type suffix).
    if (ev.action == KeyAction_Release && !reportEvents) {
        return {};
    }

    // Modifier-only keys: only report if REPORT_ALL_KEYS
    if (isModifierKey(ev.key) && !reportAllKeys) {
        return {};
    }

    // Compute Kitty modifier wire value
    uint32_t kittyMods = 0;
    if (ev.modifiers & ShiftModifier) {
        kittyMods |= 1;
    }
    if (ev.modifiers & AltModifier) {
        kittyMods |= 2;
    }
    if (ev.modifiers & CtrlModifier) {
        kittyMods |= 4;
    }
    if (ev.modifiers & MetaModifier) {
        kittyMods |= 8;
    }
    if (ev.modifiers & HyperModifier) {
        kittyMods |= 16;
    }
    if (ev.modifiers & CapsLockModifier) {
        kittyMods |= 64;
    }
    if (ev.modifiers & NumLockModifier) {
        kittyMods |= 128;
    }
    uint32_t wireMods = kittyMods + 1; // protocol adds 1

    // When REPORT_EVENT_TYPES is not set, treat repeat as press — the event
    // type is not included in the output and repeat should encode identically.
    uint32_t eventType = (!reportEvents && ev.action == KeyAction_Repeat)
        ? static_cast<uint32_t>(KeyAction_Press)
        : static_cast<uint32_t>(ev.action);

    // Determine key code. Per kitty (key_encoding.c via xkb_glfw.c:949 —
    // `glfw_ev.key = ... ?: xkb_state_key_get_utf32(...)`) the keycode is
    // the *current layout's* codepoint for the keystroke, not the physical
    // PC-101 position. So when text is available it wins over the
    // `Key_A → 'a'` shortcut: a Russian-layout user pressing the physical
    // A key produces text="ф" (U+0444), which becomes keyCode=1092. The
    // base_layout_key field carries 'a' separately.
    uint32_t funcCode = kittyFunctionalCode(ev.key);
    uint32_t keyCode  = 0;

    if (funcCode != 0) {
        keyCode = funcCode;
    } else if (!ev.text.empty()) {
        int consumed = 0;
        keyCode      = utf8::decode(ev.text.c_str(), static_cast<int>(ev.text.size()), consumed);
    } else if (ev.key >= Key_A && ev.key <= Key_Z) {
        keyCode = static_cast<uint32_t>(ev.key - Key_A + 'a'); // lowercase
    } else if (ev.key >= Key_0 && ev.key <= Key_9) {
        keyCode = static_cast<uint32_t>(ev.key);
    } else if (ev.key == Key_Space) {
        keyCode = ' ';
    } else {
        return {}; // Can't encode
    }

    // Determine if this should use legacy encoding or CSI u
    bool useCsiU = false;

    if (reportAllKeys) {
        // REPORT_ALL_KEYS: everything uses CSI u (except legacy cursor keys without mods)
        useCsiU = true;
    } else if (disambiguate) {
        // DISAMBIGUATE: use CSI u for keys that are ambiguous in legacy encoding
        if (funcCode != 0) {
            // Functional keys: check if they have a legacy form
            char legacyTrailer = kittyLegacyTrailer(ev.key);
            int tildeNum       = kittyLegacyTildeNumber(ev.key);

            if (legacyTrailer) {
                // Arrow keys, F1-F4, Home, End: use legacy CSI form with mods
                if (kittyMods == 0 && eventType == 1) {
                    // No modifiers, press event: legacy form
                    if (legacyTrailer == 'P' || legacyTrailer == 'Q' ||
                        legacyTrailer == 'R' || legacyTrailer == 'S') {
                        // F1-F4: ESC O X
                        char buf[4] = { '\x1b', 'O', legacyTrailer, '\0' };
                        return std::string(buf, 3);
                    }
                    char buf[4] = { '\x1b', '[', legacyTrailer, '\0' };
                    return std::string(buf, 3);
                }
                // With modifiers: CSI 1;mods X
                char buf[32];
                int len;
                if (reportEvents && eventType != 1) {
                    len = snprintf(buf, sizeof(buf), "\x1b[1;%u:%u%c", wireMods, eventType, legacyTrailer);
                } else {
                    len = snprintf(buf, sizeof(buf), "\x1b[1;%u%c", wireMods, legacyTrailer);
                }
                return std::string(buf, len);
            } else if (tildeNum) {
                // Insert, Delete, PageUp/Down, F5-F12: CSI number;mods ~
                char buf[32];
                int len;
                if (kittyMods == 0 && eventType == 1) {
                    len = snprintf(buf, sizeof(buf), "\x1b[%d~", tildeNum);
                } else if (reportEvents && eventType != 1) {
                    len = snprintf(buf, sizeof(buf), "\x1b[%d;%u:%u~", tildeNum, wireMods, eventType);
                } else {
                    len = snprintf(buf, sizeof(buf), "\x1b[%d;%u~", tildeNum, wireMods);
                }
                return std::string(buf, len);
            } else {
                // Other functional keys (Escape, Enter, Tab, Backspace, keypad):
                // Use CSI u if modifiers present
                if (kittyMods != 0 || eventType != 1) {
                    useCsiU = true;
                } else {
                    // No modifiers, press: use legacy text
                    switch (ev.key) {
                        case Key_Escape: return "\x1b";
                        case Key_Return:
                        case Key_Enter: return "\r";
                        case Key_Tab: return "\t";
                        case Key_Backspace: return "\x7f";
                        default: useCsiU = true; break;
                    }
                }
            }
        } else if (kittyMods != 0 || eventType != 1) {
            // Text key with modifiers or non-press event
            useCsiU = true;
        } else {
            // Plain text key, no modifiers, press: send as text
            return ev.text;
        }
    } else {
        // No relevant flags (shouldn't reach here if caller checks)
        return ev.text;
    }

    if (!useCsiU && !reportAllKeys) {
        return ev.text.empty() ? std::string {} : ev.text;
    }

    // Build CSI u sequence
    // Format: CSI keycode[:shifted_key[:alternate_key]] ; mods[:event] [;text] u
    char buf[128];
    int len;

    bool needMods  = (wireMods > 1 || (reportEvents && eventType != 1));
    bool needEvent = (reportEvents && eventType != 1);
    bool needText  = (reportText && !ev.text.empty());

    // Build key portion: keycode[:shifted_key[:base_layout_key]] per
    // kitty's `serialize` (key_encoding.c:65-77). The `add_alternates`
    // gate fires when reportAlternate is set AND either
    //   (a) shifted_key is meaningful AND shift is currently held, OR
    //   (b) base_layout_key is set and differs from keycode.
    // When firing, the leading ':' is always emitted; the shifted_key
    // sub-field is emitted only when shift is held (otherwise empty —
    // i.e. the spec's `keycode::base_layout_key` form is reachable).
    char keyPart[64];
    bool shiftHeld          = (ev.modifiers & ShiftModifier) != 0;
    bool emitShifted        = reportAlternate && shiftHeld && ev.shiftedKey != 0 && ev.shiftedKey != keyCode;
    bool emitBaseLayout     = reportAlternate && ev.baseLayoutKey != 0 && ev.baseLayoutKey != keyCode;
    if (emitShifted && emitBaseLayout) {
        snprintf(keyPart, sizeof(keyPart), "%u:%u:%u", keyCode, ev.shiftedKey, ev.baseLayoutKey);
    } else if (emitShifted) {
        snprintf(keyPart, sizeof(keyPart), "%u:%u", keyCode, ev.shiftedKey);
    } else if (emitBaseLayout) {
        snprintf(keyPart, sizeof(keyPart), "%u::%u", keyCode, ev.baseLayoutKey);
    } else {
        snprintf(keyPart, sizeof(keyPart), "%u", keyCode);
    }

    if (needText) {
        // Build text as codepoints
        std::string textCps;
        const char *p   = ev.text.c_str();
        const char *end = p + ev.text.size();
        while (p < end) {
            uint32_t cp = utf8::decodeAdvance(p, end);
            if (!textCps.empty()) {
                textCps += ':';
            }
            char cpBuf[12];
            snprintf(cpBuf, sizeof(cpBuf), "%u", cp);
            textCps += cpBuf;
        }
        if (needEvent) {
            len = snprintf(buf, sizeof(buf), "\x1b[%s;%u:%u;%su", keyPart, wireMods, eventType, textCps.c_str());
        } else if (needMods) {
            len = snprintf(buf, sizeof(buf), "\x1b[%s;%u;%su", keyPart, wireMods, textCps.c_str());
        } else {
            len = snprintf(buf, sizeof(buf), "\x1b[%s;;%su", keyPart, textCps.c_str());
        }
    } else if (needEvent) {
        len = snprintf(buf, sizeof(buf), "\x1b[%s;%u:%uu", keyPart, wireMods, eventType);
    } else if (needMods) {
        len = snprintf(buf, sizeof(buf), "\x1b[%s;%uu", keyPart, wireMods);
    } else {
        len = snprintf(buf, sizeof(buf), "\x1b[%su", keyPart);
    }

    return std::string(buf, len);
}
