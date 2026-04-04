#include "TerminalEmulator.h"
#include "Log.h"
#include <spdlog/spdlog.h>
#include <cstdio>

void TerminalEmulator::keyPressEvent(const KeyEvent *event)
{
    resetViewport();

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
    if (event->action == KeyAction_Release) return;

    std::string text = event->text;
    if (text.empty()) {
        switch (event->key) {
        case Key_Return:
        case Key_Enter:    text = "\r"; break;
        case Key_Backspace: text = "\x7f"; break;
        case Key_Tab:      text = "\t"; break;
        case Key_Escape:   text = "\x1b"; break;
        case Key_Delete:   text = "\x1b[3~"; break;
        case Key_Left:     text = mCursorKeyMode ? "\x1bOD" : "\x1b[D"; break;
        case Key_Right:    text = mCursorKeyMode ? "\x1bOC" : "\x1b[C"; break;
        case Key_Up:       text = mCursorKeyMode ? "\x1bOA" : "\x1b[A"; break;
        case Key_Down:     text = mCursorKeyMode ? "\x1bOB" : "\x1b[B"; break;
        case Key_Home:     text = mCursorKeyMode ? "\x1bOH" : "\x1b[H"; break;
        case Key_End:      text = mCursorKeyMode ? "\x1bOF" : "\x1b[F"; break;
        case Key_PageUp:   text = "\x1b[5~"; break;
        case Key_PageDown: text = "\x1b[6~"; break;
        case Key_Insert:   text = "\x1b[2~"; break;
        case Key_F1:       text = "\x1bOP"; break;
        case Key_F2:       text = "\x1bOQ"; break;
        case Key_F3:       text = "\x1bOR"; break;
        case Key_F4:       text = "\x1bOS"; break;
        case Key_F5:       text = "\x1b[15~"; break;
        case Key_F6:       text = "\x1b[17~"; break;
        case Key_F7:       text = "\x1b[18~"; break;
        case Key_F8:       text = "\x1b[19~"; break;
        case Key_F9:       text = "\x1b[20~"; break;
        case Key_F10:      text = "\x1b[21~"; break;
        case Key_F11:      text = "\x1b[23~"; break;
        case Key_F12:      text = "\x1b[24~"; break;
        default:
            break;
        }
    }
    if (text.empty()) {
        spdlog::debug("keyPressEvent: no text to send for key=0x{:x}", static_cast<int>(event->key));
    }
    if (!text.empty() && event->count) {
        for (size_t i=0; i<event->count; ++i) {
            writeToOutput(text.c_str(), text.size());
        }
    }
}

// --- Kitty keyboard protocol ---

void TerminalEmulator::kittyPushFlags(uint8_t flags)
{
    auto* stack = mUsingAltScreen ? mKittyStackAlt : mKittyStackMain;
    auto& depth = mUsingAltScreen ? mKittyStackDepthAlt : mKittyStackDepthMain;
    if (depth < KITTY_STACK_MAX) {
        stack[depth++] = flags;
    }
    mKittyFlags = flags;
}

void TerminalEmulator::kittyPopFlags(int count)
{
    auto* stack = mUsingAltScreen ? mKittyStackAlt : mKittyStackMain;
    auto& depth = mUsingAltScreen ? mKittyStackDepthAlt : mKittyStackDepthMain;
    for (int i = 0; i < count && depth > 0; i++) {
        depth--;
    }
    mKittyFlags = (depth > 0) ? stack[depth - 1] : 0;
}

void TerminalEmulator::kittySetFlags(uint8_t flags, int mode)
{
    switch (mode) {
    case 1: mKittyFlags = flags; break;         // replace
    case 2: mKittyFlags |= flags; break;        // OR
    case 3: mKittyFlags &= ~flags; break;       // AND NOT
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
    case Key_Escape:    return 27;
    case Key_Return:
    case Key_Enter:     return 13;
    case Key_Tab:       return 9;
    case Key_Backspace: return 127;
    case Key_Insert:    return 57348;
    case Key_Delete:    return 57349;
    case Key_Left:      return 57350;
    case Key_Right:     return 57351;
    case Key_Up:        return 57352;
    case Key_Down:      return 57353;
    case Key_PageUp:    return 57354;
    case Key_PageDown:  return 57355;
    case Key_Home:      return 57356;
    case Key_End:       return 57357;
    case Key_CapsLock:  return 57358;
    case Key_ScrollLock: return 57359;
    case Key_NumLock:   return 57360;
    case Key_Print:     return 57361;
    case Key_Pause:     return 57362;
    case Key_Menu:      return 57363;
    case Key_F1:        return 57364;
    case Key_F2:        return 57365;
    case Key_F3:        return 57366;
    case Key_F4:        return 57367;
    case Key_F5:        return 57368;
    case Key_F6:        return 57369;
    case Key_F7:        return 57370;
    case Key_F8:        return 57371;
    case Key_F9:        return 57372;
    case Key_F10:       return 57373;
    case Key_F11:       return 57374;
    case Key_F12:       return 57375;
    case Key_F13:       return 57376;
    case Key_F14:       return 57377;
    case Key_F15:       return 57378;
    case Key_F16:       return 57379;
    case Key_F17:       return 57380;
    case Key_F18:       return 57381;
    case Key_F19:       return 57382;
    case Key_F20:       return 57383;
    case Key_F21:       return 57384;
    case Key_F22:       return 57385;
    case Key_F23:       return 57386;
    case Key_F24:       return 57387;
    case Key_F25:       return 57388;
    case Key_KP_0:      return 57399;
    case Key_KP_1:      return 57400;
    case Key_KP_2:      return 57401;
    case Key_KP_3:      return 57402;
    case Key_KP_4:      return 57403;
    case Key_KP_5:      return 57404;
    case Key_KP_6:      return 57405;
    case Key_KP_7:      return 57406;
    case Key_KP_8:      return 57407;
    case Key_KP_9:      return 57408;
    case Key_KP_Decimal:  return 57409;
    case Key_KP_Divide:   return 57410;
    case Key_KP_Multiply: return 57411;
    case Key_KP_Subtract: return 57412;
    case Key_KP_Add:      return 57413;
    case Key_KP_Enter:    return 57414;
    case Key_KP_Equal:    return 57415;
    case Key_Shift_L:   return 57441;
    case Key_Control_L: return 57442;
    case Key_Alt_L:     return 57443;
    case Key_Super_L:   return 57444;
    case Key_Hyper_L:   return 57445;
    case Key_Shift_R:   return 57447;
    case Key_Control_R: return 57448;
    case Key_Alt_R:     return 57449;
    case Key_Super_R:   return 57450;
    case Key_Hyper_R:   return 57451;
    default:            return 0;
    }
}

static bool isModifierKey(Key k)
{
    switch (k) {
    case Key_Shift: case Key_Shift_L: case Key_Shift_R:
    case Key_Control: case Key_Control_L: case Key_Control_R:
    case Key_Alt: case Key_Alt_L: case Key_Alt_R:
    case Key_Meta: case Key_Super_L: case Key_Super_R:
    case Key_Hyper_L: case Key_Hyper_R:
    case Key_CapsLock: case Key_NumLock:
        return true;
    default:
        return false;
    }
}

// Legacy CSI trailer for keys that use the traditional CSI format
// Returns 0 if the key should use CSI u format instead
static char kittyLegacyTrailer(Key k)
{
    switch (k) {
    case Key_Up:    return 'A';
    case Key_Down:  return 'B';
    case Key_Right: return 'C';
    case Key_Left:  return 'D';
    case Key_Home:  return 'H';
    case Key_End:   return 'F';
    case Key_F1:    return 'P';
    case Key_F2:    return 'Q';
    case Key_F3:    return 'R';
    case Key_F4:    return 'S';
    default:        return 0;
    }
}

// Legacy CSI ~ number for keys that use CSI number ~ format
static int kittyLegacyTildeNumber(Key k)
{
    switch (k) {
    case Key_Insert:   return 2;
    case Key_Delete:   return 3;
    case Key_PageUp:   return 5;
    case Key_PageDown: return 6;
    case Key_F5:       return 15;
    case Key_F6:       return 17;
    case Key_F7:       return 18;
    case Key_F8:       return 19;
    case Key_F9:       return 20;
    case Key_F10:      return 21;
    case Key_F11:      return 23;
    case Key_F12:      return 24;
    default:           return 0;
    }
}

std::string TerminalEmulator::encodeKittyKey(const KeyEvent& ev) const
{
    bool disambiguate = mKittyFlags & 0x01;
    bool reportEvents = mKittyFlags & 0x02;
    bool reportAllKeys = mKittyFlags & 0x08;
    bool reportText = mKittyFlags & 0x10;

    // Drop release/repeat events unless REPORT_EVENT_TYPES is active
    if (ev.action == KeyAction_Release && !reportEvents) return {};
    if (ev.action == KeyAction_Repeat && !reportEvents) return {};

    // Modifier-only keys: only report if REPORT_ALL_KEYS
    if (isModifierKey(ev.key) && !reportAllKeys) return {};

    // Compute Kitty modifier wire value
    uint32_t kittyMods = 0;
    if (ev.modifiers & ShiftModifier)    kittyMods |= 1;
    if (ev.modifiers & AltModifier)      kittyMods |= 2;
    if (ev.modifiers & CtrlModifier)     kittyMods |= 4;
    if (ev.modifiers & MetaModifier)     kittyMods |= 8;
    if (ev.modifiers & HyperModifier)    kittyMods |= 16;
    if (ev.modifiers & CapsLockModifier) kittyMods |= 64;
    if (ev.modifiers & NumLockModifier)  kittyMods |= 128;
    uint32_t wireMods = kittyMods + 1; // protocol adds 1

    uint32_t eventType = static_cast<uint32_t>(ev.action); // 1=press, 2=repeat, 3=release

    // Determine key code
    uint32_t funcCode = kittyFunctionalCode(ev.key);
    uint32_t keyCode = 0;

    if (funcCode != 0) {
        keyCode = funcCode;
    } else if (ev.key >= Key_A && ev.key <= Key_Z) {
        keyCode = static_cast<uint32_t>(ev.key - Key_A + 'a'); // lowercase
    } else if (ev.key >= Key_0 && ev.key <= Key_9) {
        keyCode = static_cast<uint32_t>(ev.key);
    } else if (ev.key == Key_Space) {
        keyCode = ' ';
    } else if (!ev.text.empty()) {
        // Use codepoint from text
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ev.text.c_str());
        if (*p < 0x80) keyCode = *p;
        else if ((*p & 0xE0) == 0xC0) keyCode = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
        else if ((*p & 0xF0) == 0xE0) keyCode = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
        else keyCode = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);
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
            int tildeNum = kittyLegacyTildeNumber(ev.key);

            if (legacyTrailer) {
                // Arrow keys, F1-F4, Home, End: use legacy CSI form with mods
                if (kittyMods == 0 && eventType == 1) {
                    // No modifiers, press event: legacy form
                    if (legacyTrailer == 'P' || legacyTrailer == 'Q' ||
                        legacyTrailer == 'R' || legacyTrailer == 'S') {
                        // F1-F4: ESC O X
                        char buf[4] = {'\x1b', 'O', legacyTrailer, '\0'};
                        return std::string(buf, 3);
                    }
                    char buf[4] = {'\x1b', '[', legacyTrailer, '\0'};
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
                    case Key_Escape:    return "\x1b";
                    case Key_Return:
                    case Key_Enter:     return "\r";
                    case Key_Tab:       return "\t";
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
        return ev.text.empty() ? std::string{} : ev.text;
    }

    // Build CSI u sequence
    char buf[64];
    int len;

    bool needMods = (wireMods > 1 || (reportEvents && eventType != 1));
    bool needEvent = (reportEvents && eventType != 1);
    bool needText = (reportText && !ev.text.empty());

    if (needText) {
        // Build text as codepoints
        std::string textCps;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ev.text.c_str());
        const uint8_t* end = p + ev.text.size();
        while (p < end) {
            uint32_t cp;
            if (*p < 0x80) cp = *p++;
            else if ((*p & 0xE0) == 0xC0) { cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F); p += 2; }
            else if ((*p & 0xF0) == 0xE0) { cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F); p += 3; }
            else { cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F); p += 4; }
            if (!textCps.empty()) textCps += ':';
            char cpBuf[12];
            snprintf(cpBuf, sizeof(cpBuf), "%u", cp);
            textCps += cpBuf;
        }
        if (needEvent) {
            len = snprintf(buf, sizeof(buf), "\x1b[%u;%u:%u;%su", keyCode, wireMods, eventType, textCps.c_str());
        } else if (needMods) {
            len = snprintf(buf, sizeof(buf), "\x1b[%u;%u;%su", keyCode, wireMods, textCps.c_str());
        } else {
            len = snprintf(buf, sizeof(buf), "\x1b[%u;;%su", keyCode, textCps.c_str());
        }
    } else if (needEvent) {
        len = snprintf(buf, sizeof(buf), "\x1b[%u;%u:%uu", keyCode, wireMods, eventType);
    } else if (needMods) {
        len = snprintf(buf, sizeof(buf), "\x1b[%u;%uu", keyCode, wireMods);
    } else {
        len = snprintf(buf, sizeof(buf), "\x1b[%uu", keyCode);
    }

    return std::string(buf, len);
}
